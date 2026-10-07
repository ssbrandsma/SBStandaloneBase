local tonumber = tonumber
local string = require("string")

local State = {}

local labels = {
    ACTIVE = "Active", AVAILABLE = "Available", UNAVAILABLE = "Not available",
    UNSUPPORTED = "Not available", ERROR = "Error", REBOOT_REQUIRED = "Reboot required",
}

local stages = {
    CHECKING_SYSTEM = "Checking system...", PREPARING_STORAGE = "Preparing storage...",
    LOADING_DRIVER = "Loading storage driver...",
    INITIALIZING_FILESYSTEM = "Initializing filesystem...",
    MOUNTING = "Mounting extended storage...", COPYING_APPLETS = "Copying applets...",
    VERIFYING_APPLETS = "Verifying applets...",
    INSTALLING_BOOT_SUPPORT = "Installing boot support...", FINISHING = "Finishing...",
    COMPLETE = "Complete",
}

local errors = {
    [20] = "This Radio configuration is not supported.",
    [21] = "Extended Storage is not available.",
    [22] = "The Extended Storage driver could not be loaded.",
    [23] = "The Extended Storage filesystem could not be initialized.",
    [24] = "Extended Storage could not be mounted.",
    [25] = "The applets could not be copied.",
    [26] = "Applet migration verification failed.",
    [27] = "Boot support could not be installed.",
}

function State.statusLabel(value) return labels[value] or "Error" end
function State.stageText(value) return stages[value] end
function State.errorText(value, reason)
    if reason == "physical_validation_safety_gate" then
        return "The image passed all checks. Initialization remains locked pending physical validation."
    elseif reason == "sbdata_image_missing" or reason == "sbdata_image_identity_mismatch" then
        return "The Extended Storage initialization image is missing or invalid."
    end
    return errors[tonumber(value)] or "Extended Storage initialization failed. Details are in /tmp/sbstorage.log."
end
function State.mibFromKb(value) return string.format("%.1f MB", (tonumber(value) or 0) / 1024) end

return State
