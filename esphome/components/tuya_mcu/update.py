"""Tuya MCU firmware update entity.

The firmware image is fetched at *compile time* from an ESPHome-style release
manifest (URL or local file) and embedded in the ESPHome binary. At runtime
the entity compares the bundled version with the version the MCU reports in a
string datapoint and, when installed, streams the image to the MCU with the
standard Tuya MCU upgrade protocol (0x0A start / 0x0B data).

Compile-time bundling is used because `http_request` is not available on
BK72xx (LibreTiny). To pick up a new MCU release, recompile/OTA ESPHome.
"""
import hashlib
import json
import logging
from pathlib import Path
import urllib.parse
import urllib.request

import esphome.codegen as cg
from esphome.components import uart, update
from esphome.components.tuya import CONF_TUYA_ID, Tuya
import esphome.config_validation as cv
from esphome.const import CONF_RAW_DATA_ID, CONF_SOURCE
from esphome.core import CORE

from . import tuya_mcu_ns

_LOGGER = logging.getLogger(__name__)

DEPENDENCIES = ["tuya", "uart"]

CONF_VERSION_DATAPOINT = "version_datapoint"
CONF_CHIP_FAMILY = "chip_family"
CONF_REQUIRED = "required"
CONF_PACKET_TIMEOUT = "packet_timeout"
CONF_START_TIMEOUT = "start_timeout"
CONF_VERIFY_TIMEOUT = "verify_timeout"
CONF_RETRIES = "retries"

TuyaMcuUpdate = tuya_mcu_ns.class_(
    "TuyaMcuUpdate", update.UpdateEntity, cg.Component, uart.UARTDevice
)


def _source(value):
    value = cv.string_strict(value)
    if value.startswith(("http://", "https://")):
        return cv.url(value)
    return value


CONFIG_SCHEMA = (
    update.update_schema(TuyaMcuUpdate)
    .extend(
        {
            cv.GenerateID(CONF_TUYA_ID): cv.use_id(Tuya),
            cv.GenerateID(CONF_RAW_DATA_ID): cv.declare_id(cg.uint8),
            cv.Required(CONF_SOURCE): _source,
            cv.Optional(CONF_REQUIRED, default=False): cv.boolean,
            cv.Optional(CONF_CHIP_FAMILY, default="MS51FC0AE"): cv.string,
            cv.Optional(CONF_VERSION_DATAPOINT, default=120): cv.uint8_t,
            cv.Optional(CONF_START_TIMEOUT, default="8s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_PACKET_TIMEOUT, default="5s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_VERIFY_TIMEOUT, default="60s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_RETRIES, default=3): cv.int_range(min=0, max=10),
        }
    )
    .extend(uart.UART_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


def _read(src: str, base: Path | None) -> tuple[bytes, str]:
    """Return (content, resolved location) for a URL or a file path."""
    if src.startswith(("http://", "https://")):
        req = urllib.request.Request(src, headers={"User-Agent": "esphome-tuya_mcu"})
        with urllib.request.urlopen(req, timeout=30) as r:  # follows redirects
            return r.read(), src
    p = Path(src)
    if not p.is_absolute():
        p = (base or Path(CORE.relative_config_path("."))) / p
    return p.read_bytes(), str(p.resolve())


def _join(location: str, path: str) -> str:
    if path.startswith(("http://", "https://")) or Path(path).is_absolute():
        return path
    if location.startswith(("http://", "https://")):
        return urllib.parse.urljoin(location, path)
    return str(Path(location).parent / path)


def fetch_firmware(source: str, chip_family: str) -> dict:
    raw, loc = _read(source, None)
    manifest = json.loads(raw)
    for build in manifest.get("builds", []):
        if build.get("chipFamily") != chip_family:
            continue
        ota = build["ota"]
        url = _join(loc, ota["path"])
        cache = Path(CORE.relative_internal_path("tuya_mcu")) / (ota["md5"] + ".bin")
        if cache.is_file():
            data = cache.read_bytes()
        else:
            data, _ = _read(url, None)
        if hashlib.md5(data).hexdigest() != ota["md5"].lower():
            raise cv.Invalid(f"MD5 mismatch for {url}")
        cache.parent.mkdir(parents=True, exist_ok=True)
        cache.write_bytes(data)
        return {
            "data": data,
            "version": str(manifest["version"]),
            "title": manifest.get("name", ""),
            "md5": ota["md5"].lower(),
            "release_url": ota.get("release_url", ""),
            "summary": ota.get("summary", ""),
            "url": url,
        }
    raise cv.Invalid(f"No build for chipFamily '{chip_family}' in {source}")


async def to_code(config):
    var = await update.new_update(config)
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    parent = await cg.get_variable(config[CONF_TUYA_ID])
    cg.add(var.set_tuya_parent(parent))
    cg.add(var.set_version_datapoint(config[CONF_VERSION_DATAPOINT]))
    cg.add(var.set_timeouts(config[CONF_START_TIMEOUT], config[CONF_PACKET_TIMEOUT],
                            config[CONF_VERIFY_TIMEOUT], config[CONF_RETRIES]))

    try:
        fw = fetch_firmware(config[CONF_SOURCE], config[CONF_CHIP_FAMILY])
    except Exception as err:  # noqa: BLE001
        if config[CONF_REQUIRED]:
            raise cv.Invalid(f"tuya_mcu: could not fetch MCU firmware: {err}") from err
        _LOGGER.warning("tuya_mcu: no MCU firmware bundled (%s): %s", config[CONF_SOURCE], err)
        return

    _LOGGER.info("tuya_mcu: bundling MCU firmware %s (%d bytes) from %s",
                 fw["version"], len(fw["data"]), fw["url"])
    arr = cg.progmem_array(config[CONF_RAW_DATA_ID], list(fw["data"]))
    cg.add(var.set_firmware(arr, len(fw["data"]), fw["version"], fw["title"], fw["md5"],
                            fw["release_url"], fw["summary"]))
