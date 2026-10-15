"""Text sensors exposing information about the Shelly emulator itself."""

import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC, ICON_CHIP
from esphome.types import ConfigType

from .. import CONF_SHELLY_EMU_ID, ShellyEmu

DEPENDENCIES = ["shelly_emu"]

CONF_COMPONENT_VERSION = "component_version"
CONF_DEVICE_ID = "device_id"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_SHELLY_EMU_ID): cv.use_id(ShellyEmu),
        cv.Optional(CONF_COMPONENT_VERSION): text_sensor.text_sensor_schema(
            icon=ICON_CHIP, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ),
        cv.Optional(CONF_DEVICE_ID): text_sensor.text_sensor_schema(
            icon="mdi:identifier", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ),
    }
)


async def to_code(config: ConfigType) -> None:
    parent = await cg.get_variable(config[CONF_SHELLY_EMU_ID])
    if CONF_COMPONENT_VERSION in config:
        sens = await text_sensor.new_text_sensor(config[CONF_COMPONENT_VERSION])
        cg.add(parent.set_version_text_sensor(sens))
    if CONF_DEVICE_ID in config:
        sens = await text_sensor.new_text_sensor(config[CONF_DEVICE_ID])
        cg.add(parent.set_device_id_text_sensor(sens))
