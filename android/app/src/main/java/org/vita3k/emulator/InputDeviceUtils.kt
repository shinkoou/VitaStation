package org.vita3k.emulator

import android.view.InputDevice

enum class ControllerFamily {
    Standard,
    Xbox,
    Nintendo
}

data class ConnectedGamepad(
    val deviceId: Int,
    val name: String,
    val family: ControllerFamily
)

object InputDeviceUtils {
    @JvmStatic
    fun isPhysicalGamepad(device: InputDevice?): Boolean {
        if (device == null || device.isVirtual) {
            return false
        }

        val name = device.name.orEmpty()
        if (name.startsWith("uinput-") || name.startsWith("gf_")) {
            return false
        }

        val sources = device.sources
        return (sources and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
            (sources and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
    }

    @JvmStatic
    fun describeDevice(device: InputDevice?): String {
        if (device == null) {
            return "[VS-PAD] device=null"
        }
        return "[VS-PAD] id=${device.id} name=${device.name.orEmpty()} " +
            "vendor=${device.vendorId} product=${device.productId} sources=0x${device.sources.toString(16)}"
    }

    @JvmStatic
    fun hasPhysicalGamepadConnected(): Boolean {
        return getPhysicalGamepads().isNotEmpty()
    }

    @JvmStatic
    fun getPhysicalGamepads(): List<ConnectedGamepad> {
        val controllers = mutableListOf<ConnectedGamepad>()
        val deviceIds = InputDevice.getDeviceIds()
        for (deviceId in deviceIds) {
            val device = InputDevice.getDevice(deviceId) ?: continue
            if (isPhysicalGamepad(device)) {
                val name = device.name.orEmpty()
                controllers += ConnectedGamepad(
                    deviceId = deviceId,
                    name = name,
                    family = inferControllerFamily(name)
                )
            }
        }

        return controllers.sortedBy { it.deviceId }
    }

    private fun inferControllerFamily(name: String): ControllerFamily {
        val normalized = name.lowercase()
        return when {
            normalized.contains("xbox") || normalized.contains("x-input") -> ControllerFamily.Xbox
            normalized.contains("switch") || normalized.contains("joy-con") || normalized.contains("nintendo") -> ControllerFamily.Nintendo
            else -> ControllerFamily.Standard
        }
    }
}
