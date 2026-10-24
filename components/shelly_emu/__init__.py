"""Shelly Gen2 device emulator for ESPHome.

Makes an ESP32 look like a Shelly Gen2 switch/plug (Pro 1PM/2PM/4PM, Plus 1PM/2PM,
Plus Plug S) or a Shelly Pro 3EM energy meter on the LAN, so that controllers such as
Sigenergy SigenStor / mySigen can discover and control it. All channel data comes from
ESPHome entities, typically ``homeassistant`` platform switches and sensors.
"""

import esphome.codegen as cg
from esphome.components import binary_sensor, sensor, socket, switch, web_server_base
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID
import esphome.config_validation as cv
from esphome.const import (
    CONF_CURRENT,
    CONF_ENERGY,
    CONF_FREQUENCY,
    CONF_ID,
    CONF_INPUT,
    CONF_MODEL,
    CONF_NAME,
    CONF_POWER,
    CONF_POWER_FACTOR,
    CONF_SWITCH,
    CONF_TEMPERATURE,
    CONF_TIMEZONE,
    CONF_VOLTAGE,
)
from esphome.types import ConfigType

CODEOWNERS = ["@Qonstrukt"]
DEPENDENCIES = ["esp32", "network"]

__version__ = "1.0.1"

CONF_MAC = "mac"
CONF_MODEL_CODE = "model_code"
CONF_FIRMWARE_VERSION = "firmware_version"
CONF_FW_ID = "fw_id"
CONF_BLE = "ble"
CONF_BLE_PROVISION_STATE = "ble_provision_state"
CONF_BLE_SERVER_ID = "ble_server_id"
CONF_MODBUS = "modbus"
CONF_MODBUS_PORT = "modbus_port"
CONF_SWITCHES = "switches"
CONF_EM = "em"
CONF_PHASES = "phases"
CONF_ENERGY_RETURNED = "energy_returned"
CONF_APPARENT_POWER = "apparent_power"
CONF_NEUTRAL_CURRENT = "neutral_current"
CONF_ENERGY_UNIT = "energy_unit"
CONF_OUTPUT_ADDON = "output_addon"
CONF_SHELLY_EMU_ID = "shelly_emu_id"

shelly_emu_ns = cg.esphome_ns.namespace("shelly_emu")
ShellyEmu = shelly_emu_ns.class_("ShellyEmu", cg.Component)
DeviceKind = shelly_emu_ns.enum("DeviceKind", is_class=True)

# model key -> (model code, app, hostname prefix, switch channels, is_pro, is_meter)
MODELS = {
    "pro1pm": ("SPSW-201PE16EU", "Pro1PM", "ShellyPro1PM", 1, True, False),
    "pro2pm": ("SPSW-202PE16EU", "Pro2PM", "ShellyPro2PM", 2, True, False),
    "pro4pm": ("SPSW-004PE16EU", "Pro4PM", "ShellyPro4PM", 4, True, False),
    "plus1pm": ("SNSW-001P16EU", "Plus1PM", "ShellyPlus1PM", 1, False, False),
    "plus2pm": ("SNSW-102P16EU", "Plus2PM", "ShellyPlus2PM", 2, False, False),
    "plusplugs": ("SNPL-00112EU", "PlusPlugS", "ShellyPlusPlugS", 1, False, False),
    "pro3em": ("SPEM-003CEBEU", "Pro3EM", "ShellyPro3EM", 0, True, True),
}

ENERGY_UNITS = {"kWh": 1000.0, "Wh": 1.0}


def AUTO_LOAD(config: ConfigType) -> list[str]:
    loads = ["web_server_base", "mdns", "json", "socket", "sensor", "binary_sensor", "switch"]
    if config.get(CONF_BLE):
        loads += ["esp32_ble", "esp32_ble_server"]
    return loads


def _validate_mac(value):
    value = cv.string(value).replace(":", "").replace("-", "").upper()
    if len(value) != 12 or any(c not in "0123456789ABCDEF" for c in value):
        raise cv.Invalid("mac must be 12 hex digits, e.g. AABBCCDDEEFF")
    return value


SWITCH_SENSOR_SETTERS = {
    CONF_POWER: "set_switch_power",
    CONF_VOLTAGE: "set_switch_voltage",
    CONF_CURRENT: "set_switch_current",
    CONF_ENERGY: "set_switch_energy",
    CONF_ENERGY_RETURNED: "set_switch_energy_returned",
    CONF_FREQUENCY: "set_switch_frequency",
    CONF_POWER_FACTOR: "set_switch_power_factor",
    CONF_TEMPERATURE: "set_switch_temperature",
}

PHASE_SENSOR_SETTERS = {
    CONF_VOLTAGE: "set_em_voltage",
    CONF_CURRENT: "set_em_current",
    CONF_POWER: "set_em_power",
    CONF_APPARENT_POWER: "set_em_apparent_power",
    CONF_POWER_FACTOR: "set_em_power_factor",
    CONF_FREQUENCY: "set_em_frequency",
    CONF_ENERGY: "set_em_energy",
    CONF_ENERGY_RETURNED: "set_em_energy_returned",
}

SWITCH_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_SWITCH): cv.use_id(switch.Switch),
        cv.Optional(CONF_NAME): cv.string,
        cv.Optional(CONF_INPUT): cv.use_id(binary_sensor.BinarySensor),
        cv.Optional(CONF_ENERGY_UNIT, default="kWh"): cv.one_of(*ENERGY_UNITS),
        **{cv.Optional(key): cv.use_id(sensor.Sensor) for key in SWITCH_SENSOR_SETTERS},
    }
)

PHASE_SCHEMA = cv.Schema(
    {cv.Optional(key): cv.use_id(sensor.Sensor) for key in PHASE_SENSOR_SETTERS}
)

OUTPUT_ADDON_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_SWITCH): cv.use_id(switch.Switch),
        cv.Optional(CONF_NAME): cv.string,
    }
)

EM_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_PHASES): cv.All(
            cv.ensure_list(PHASE_SCHEMA), cv.Length(min=1, max=3)
        ),
        cv.Optional(CONF_NEUTRAL_CURRENT): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_TEMPERATURE): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_ENERGY_UNIT, default="kWh"): cv.one_of(*ENERGY_UNITS),
    }
)


def _validate_model(config: ConfigType) -> ConfigType:
    _, _, _, channels, _, is_meter = MODELS[config[CONF_MODEL]]
    if is_meter:
        if CONF_EM not in config:
            raise cv.Invalid(f"model '{config[CONF_MODEL]}' requires an 'em' section")
        if config[CONF_SWITCHES]:
            raise cv.Invalid(
                f"model '{config[CONF_MODEL]}' has no switch channels; use 'output_addon' for the Pro Output Add-on relay"
            )
    else:
        if CONF_EM in config:
            raise cv.Invalid(f"model '{config[CONF_MODEL]}' is not an energy meter")
        if CONF_OUTPUT_ADDON in config:
            raise cv.Invalid("'output_addon' is only available for model 'pro3em'")
        if not config[CONF_SWITCHES]:
            raise cv.Invalid("at least one entry in 'switches' is required")
        if len(config[CONF_SWITCHES]) > channels:
            raise cv.Invalid(
                f"model '{config[CONF_MODEL]}' supports at most {channels} switch channel(s)"
            )
    if CONF_MODBUS not in config:
        config = config.copy()
        config[CONF_MODBUS] = is_meter
    return config


def _ble_ids(config: ConfigType) -> ConfigType:
    if not config[CONF_BLE]:
        return config
    from esphome.components import esp32_ble, esp32_ble_server

    return cv.Schema(
        {
            cv.GenerateID(esp32_ble.CONF_BLE_ID): cv.use_id(esp32_ble.ESP32BLE),
            cv.GenerateID(CONF_BLE_SERVER_ID): cv.use_id(esp32_ble_server.BLEServer),
        },
        extra=cv.ALLOW_EXTRA,
    )(config)


def _consume_sockets(config: ConfigType) -> ConfigType:
    if config[CONF_MODBUS]:
        socket.consume_sockets(1, "shelly_emu", socket.SocketType.TCP_LISTEN)(config)
        socket.consume_sockets(4, "shelly_emu", socket.SocketType.TCP)(config)
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ShellyEmu),
            cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(
                web_server_base.WebServerBase
            ),
            cv.Required(CONF_MODEL): cv.one_of(*MODELS, lower=True),
            cv.Optional(CONF_NAME): cv.string,
            cv.Optional(CONF_MAC): _validate_mac,
            cv.Optional(CONF_MODEL_CODE): cv.string,
            cv.Optional(CONF_FIRMWARE_VERSION, default="2.0.0"): cv.string,
            cv.Optional(
                CONF_FW_ID, default="20260710-101218/2.0.0-g87fbfa4"
            ): cv.string,
            cv.Optional(CONF_TIMEZONE, default="UTC"): cv.string,
            cv.Optional(CONF_BLE, default=False): cv.boolean,
            cv.Optional(CONF_BLE_PROVISION_STATE, default="pending"): cv.one_of(
                "pending", "complete", lower=True
            ),
            cv.Optional(CONF_MODBUS): cv.boolean,
            cv.Optional(CONF_MODBUS_PORT, default=502): cv.port,
            cv.Optional(CONF_SWITCHES, default=[]): cv.ensure_list(SWITCH_SCHEMA),
            cv.Optional(CONF_EM): EM_SCHEMA,
            cv.Optional(CONF_OUTPUT_ADDON): OUTPUT_ADDON_SCHEMA,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_model,
    _ble_ids,
    _consume_sockets,
)


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add_define("SHELLY_EMU_VERSION", __version__)

    base = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
    cg.add(var.set_web_server_base(base))

    code, app, prefix, _, is_pro, is_meter = MODELS[config[CONF_MODEL]]
    kind = DeviceKind.ENERGY_METER if is_meter else DeviceKind.SWITCHES
    cg.add(var.set_model(config.get(CONF_MODEL_CODE, code), app, prefix, kind, is_pro))
    cg.add(var.set_firmware(config[CONF_FIRMWARE_VERSION], config[CONF_FW_ID]))
    cg.add(var.set_timezone(config[CONF_TIMEZONE]))
    if CONF_NAME in config:
        cg.add(var.set_device_name(config[CONF_NAME]))
    if CONF_MAC in config:
        cg.add(var.set_mac_override(config[CONF_MAC]))

    for index, sw_conf in enumerate(config[CONF_SWITCHES]):
        sw = await cg.get_variable(sw_conf[CONF_SWITCH])
        cg.add(var.add_switch_channel(sw, sw_conf.get(CONF_NAME, "")))
        for key, setter in SWITCH_SENSOR_SETTERS.items():
            if key in sw_conf:
                sens = await cg.get_variable(sw_conf[key])
                cg.add(getattr(var, setter)(index, sens))
        if CONF_INPUT in sw_conf:
            inp = await cg.get_variable(sw_conf[CONF_INPUT])
            cg.add(var.set_switch_input(index, inp))
        cg.add(var.set_switch_energy_scale(index, ENERGY_UNITS[sw_conf[CONF_ENERGY_UNIT]]))

    if CONF_EM in config:
        em = config[CONF_EM]
        for phase, phase_conf in enumerate(em[CONF_PHASES]):
            for key, setter in PHASE_SENSOR_SETTERS.items():
                if key in phase_conf:
                    sens = await cg.get_variable(phase_conf[key])
                    cg.add(getattr(var, setter)(phase, sens))
        if CONF_NEUTRAL_CURRENT in em:
            sens = await cg.get_variable(em[CONF_NEUTRAL_CURRENT])
            cg.add(var.set_em_neutral_current(sens))
        if CONF_TEMPERATURE in em:
            sens = await cg.get_variable(em[CONF_TEMPERATURE])
            cg.add(var.set_em_temperature(sens))
        cg.add(var.set_em_energy_scale(ENERGY_UNITS[em[CONF_ENERGY_UNIT]]))

    if CONF_OUTPUT_ADDON in config:
        addon = config[CONF_OUTPUT_ADDON]
        sw = await cg.get_variable(addon[CONF_SWITCH])
        cg.add(var.add_switch_channel(sw, addon.get(CONF_NAME, "")))
        cg.add(var.set_output_addon(True))

    if config[CONF_BLE]:
        from esphome.components import esp32_ble

        cg.add_define("USE_SHELLY_EMU_BLE")
        ble = await cg.get_variable(config[esp32_ble.CONF_BLE_ID])
        server = await cg.get_variable(config[CONF_BLE_SERVER_ID])
        cg.add(var.set_ble_server(server))
        cg.add(var.set_ble_provision_state(config[CONF_BLE_PROVISION_STATE]))
        esp32_ble.register_gatts_event_handler(ble, var)
        esp32_ble.register_gap_event_handler(ble, var)

    if config[CONF_MODBUS]:
        cg.add_define("USE_SHELLY_EMU_MODBUS")
        cg.add(var.set_modbus_port(config[CONF_MODBUS_PORT]))
