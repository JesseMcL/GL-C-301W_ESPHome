import esphome.codegen as cg
from esphome.components import output
from esphome.components.tuya import CONF_TUYA_ID, Tuya
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_MAX_VALUE

from . import tuya_mcu_ns

DEPENDENCIES = ["tuya"]

CONF_DATAPOINT = "datapoint"
CONF_MIN_INTERVAL = "min_interval"

TuyaMcuOutput = tuya_mcu_ns.class_("TuyaMcuOutput", output.FloatOutput, cg.Component)

CONFIG_SCHEMA = output.FLOAT_OUTPUT_SCHEMA.extend(
    {
        cv.Required(CONF_ID): cv.declare_id(TuyaMcuOutput),
        cv.GenerateID(CONF_TUYA_ID): cv.use_id(Tuya),
        cv.Required(CONF_DATAPOINT): cv.uint8_t,
        cv.Optional(CONF_MAX_VALUE, default=65535): cv.int_range(min=1, max=0x7FFFFFFF),
        # Minimum spacing between ANY two tuya_mcu output datapoint writes.
        # Each write is one request/response on the UART; spacing them keeps the
        # tuya command queue from growing during light transitions.
        cv.Optional(CONF_MIN_INTERVAL, default="15ms"): cv.positive_time_period_milliseconds,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await output.register_output(var, config)
    parent = await cg.get_variable(config[CONF_TUYA_ID])
    cg.add(var.set_tuya_parent(parent))
    cg.add(var.set_datapoint(config[CONF_DATAPOINT]))
    cg.add(var.set_max_value(config[CONF_MAX_VALUE]))
    cg.add(var.set_min_interval(config[CONF_MIN_INTERVAL]))
