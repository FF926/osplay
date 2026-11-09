#include "vdd_device.hpp"

#include "helper_constants.hpp"

#include <osplay/win/vdd_device_lookup.hpp>

#include <windows.h>

#include <cfgmgr32.h>
#include <newdev.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace osplay::helper {
namespace {

struct KeyCloser {
    void operator()(HKEY key) const noexcept { RegCloseKey(key); }
};
using Key = std::unique_ptr<std::remove_pointer_t<HKEY>, KeyCloser>;

/// From its next start the device runs in a driver host process of its own: the value the INF
/// directive UmdfHostProcessSharing = ProcessSharingDisabled writes, and a device group of its own
/// (the INF puts every device in "MttVDDGroup", which keeps them in one process regardless).
bool isolate_host_process(HDEVINFO set, SP_DEVINFO_DATA& data, const std::wstring& instance) {
    HKEY raw_parameters =
        SetupDiCreateDevRegKeyW(set, &data, DICS_FLAG_GLOBAL, 0, DIREG_DEV, nullptr, nullptr);
    if (raw_parameters == INVALID_HANDLE_VALUE) {
        return false;
    }
    const Key parameters(raw_parameters);
    HKEY raw_wudf = nullptr;
    if (RegCreateKeyExW(parameters.get(), kWudfSubKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                        &raw_wudf, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const Key wudf(raw_wudf);
    const DWORD disabled = kHostProcessSharingDisabled;
    if (RegSetValueExW(wudf.get(), kHostProcessSharingValue, 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&disabled),
                       sizeof(disabled)) != ERROR_SUCCESS) {
        return false;
    }
    std::wstring group = kDeviceGroupIdPrefix + instance;
    std::ranges::replace(group, L'\\', L'.');
    const auto group_bytes = static_cast<DWORD>((group.size() + 1) * sizeof(wchar_t));
    return RegSetValueExW(wudf.get(), kDeviceGroupIdValue, 0, REG_SZ,
                          reinterpret_cast<const BYTE*>(group.c_str()),
                          group_bytes) == ERROR_SUCCESS;
}

std::optional<std::wstring> instance_id(HDEVINFO set, SP_DEVINFO_DATA& data) {
    std::wstring id(MAX_DEVICE_ID_LEN, L'\0');
    DWORD length = 0;
    if (!SetupDiGetDeviceInstanceIdW(set, &data, id.data(), static_cast<DWORD>(id.size()),
                                     &length) ||
        length == 0) {
        return std::nullopt;
    }
    id.resize(length - 1); // the reported length includes the terminator
    return id;
}

} // namespace

const std::vector<std::wstring>& VddDevices::instances() {
    const std::vector<std::wstring> present = win::device_instances(win::kVddHardwareId);
    std::erase_if(order_, [&](const std::wstring& known) {
        return std::ranges::find(present, known) == present.end();
    });
    for (const std::wstring& instance : present) {
        if (std::ranges::find(order_, instance) == order_.end()) {
            order_.push_back(instance);
            isolate(instance);
        }
    }
    return order_;
}

void VddDevices::isolate(const std::wstring& instance) {
    auto device = win::open_device(instance);
    if (!device || !isolate_host_process(device->set.get(), device->data, instance)) {
        log_.warning("Could not give a virtual display device its own driver process");
    }
}

bool VddDevices::is_installed() {
    return !instances().empty();
}

std::size_t VddDevices::count() {
    return instances().size();
}

bool VddDevices::add() {
    HDEVINFO raw_set = SetupDiCreateDeviceInfoList(&win::kDisplayDeviceClass, nullptr);
    if (raw_set == INVALID_HANDLE_VALUE) {
        log_.error("SetupAPI device list failed, error " + std::to_string(GetLastError()));
        return false;
    }
    const win::DeviceInfoSet set(raw_set);
    SP_DEVINFO_DATA data{};
    data.cbSize = sizeof(data);
    // A root device like the driver's installer makes: ROOT\DISPLAY\nnnn with the driver's ID.
    if (!SetupDiCreateDeviceInfoW(set.get(), L"DISPLAY", &win::kDisplayDeviceClass, nullptr,
                                  nullptr, DICD_GENERATE_ID, &data)) {
        log_.error("SetupAPI device creation failed, error " + std::to_string(GetLastError()));
        return false;
    }
    // REG_MULTI_SZ: the ID, its terminator and the list's terminator.
    std::wstring hardware_ids(win::kVddHardwareId);
    hardware_ids.append(2, L'\0');
    if (!SetupDiSetDeviceRegistryPropertyW(
            set.get(), &data, SPDRP_HARDWAREID, reinterpret_cast<const BYTE*>(hardware_ids.data()),
            static_cast<DWORD>(hardware_ids.size() * sizeof(wchar_t))) ||
        !SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set.get(), &data)) {
        log_.error("SetupAPI device registration failed, error " + std::to_string(GetLastError()));
        return false;
    }
    BOOL reboot = FALSE;
    if (!DiInstallDevice(nullptr, set.get(), &data, nullptr, DIIDFLAG_NOFINISHINSTALLUI, &reboot)) {
        const DWORD error = GetLastError();
        // A registered device without its driver would stay behind as an unknown device.
        static_cast<void>(SetupDiCallClassInstaller(DIF_REMOVE, set.get(), &data));
        log_.error("Installing the driver on a new device failed, error " + std::to_string(error));
        return false;
    }
    if (reboot != FALSE) {
        log_.warning("Windows asks for a reboot to finish adding a virtual display device");
    }
    // Installing wrote the INF's shared device group and started the device with it; its own group
    // applies from its next start. Restarting it now stalls for a minute, so devices added in one
    // request share a process until then (issue #30).
    const auto instance = instance_id(set.get(), data);
    if (!instance || !isolate_host_process(set.get(), data, *instance)) {
        log_.warning("Could not give the new virtual display device its own driver process");
    }
    return true;
}

bool VddDevices::is_enabled(std::size_t index) {
    const auto& known = instances();
    if (index >= known.size()) {
        return false;
    }
    const auto device = win::open_device(known[index]);
    if (!device) {
        return false;
    }
    ULONG status = 0;
    ULONG problem = 0;
    if (CM_Get_DevNode_Status(&status, &problem, device->data.DevInst, 0) != CR_SUCCESS) {
        return false;
    }
    // A device with any problem state (failed start, stuck removal, ...) cannot stream, and
    // SetupAPI changes on it can stall the helper for minutes. Leave such devices alone; the
    // driver's installer or a reboot sorts them out.
    if ((status & DN_HAS_PROBLEM) != 0) {
        return false;
    }
    return true;
}

bool VddDevices::set_enabled(std::size_t index, bool enabled) {
    const auto& known = instances();
    if (index >= known.size()) {
        log_.error("Virtual Display Driver device " + std::to_string(index) + " not found");
        return false;
    }
    const std::wstring instance = known[index];
    for (int attempt = 1; attempt <= kDeviceChangeAttempts; ++attempt) {
        if (try_set_enabled(instance, enabled)) {
            return true;
        }
        if (attempt < kDeviceChangeAttempts) {
            Sleep(kDeviceChangeRetryDelayMilliseconds);
        }
    }
    return false;
}

bool VddDevices::try_set_enabled(const std::wstring& instance, bool enabled) {
    auto device = win::open_device(instance);
    if (!device) {
        log_.error("Virtual Display Driver device not found");
        return false;
    }

    SP_PROPCHANGE_PARAMS params{};
    params.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    params.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    params.StateChange = enabled ? DICS_ENABLE : DICS_DISABLE;
    params.Scope = DICS_FLAG_GLOBAL;
    params.HwProfile = 0;

    if (!SetupDiSetClassInstallParamsW(device->set.get(), &device->data, &params.ClassInstallHeader,
                                       sizeof(params)) ||
        !SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, device->set.get(), &device->data)) {
        log_.error("SetupAPI property change failed, error " + std::to_string(GetLastError()));
        return false;
    }
    return true;
}

} // namespace osplay::helper
