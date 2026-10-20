# SPDX-License-Identifier: Apache-2.0
"""Parse every message core/reporting/ builds with Python's own JSON parser.

Runs the dump program given as the only argument (tests/unit/mqtt_messages_dump.c)
and reads its lines, "<kind>\t<label>\t<topic>\t<payload>". Beyond "it parses",
it checks what a parser that is not ours can see and the C tests cannot:

  - strict JSON: no NaN or Infinity, no duplicate key, no raw control
    character inside a string;
  - every state message has the same keys in the same order, a value only
    beside "ok", and only the status and accuracy words the ADR lists;
  - every config's "~" expands to the topics its device publishes, each
    template names only keys the state message has, its topic and unique id
    agree with its object id, and it stays under 1024 bytes;
  - nothing but the co2 entity mentions CO2 (requirements 13.3);
  - escaped text comes back as it went in.

Exit status 0 when every check passed; each failure is printed.
"""

import json
import re
import subprocess
import sys

STATUS_WORDS = {"ok", "warmup", "stale", "error", "unavailable"}
ACCURACY_WORDS = ["unreliable", "low", "medium", "high"]
CONFIG_MAX = 1024
# mqtt_messages_dump.c's ESCAPING_NAME.
ESCAPING_NAME = 'Air "monitor" \\ tab\t nl\n bell\x07 é'
LABELS = {"bsec", "plain", "statuses", "escaping", "longest"}
ENTITIES = 9

failures = []


def fail(message):
    failures.append(message)


def no_constants(name):
    raise ValueError(f"non-standard JSON constant {name}")


def no_duplicates(pairs):
    keys = [k for k, _ in pairs]
    if len(keys) != len(set(keys)):
        raise ValueError(f"duplicate key in {keys}")
    return dict(pairs)


def parse(payload, where):
    try:
        return json.loads(payload, parse_constant=no_constants, object_pairs_hook=no_duplicates)
    except ValueError as error:
        fail(f"{where}: does not parse: {error}: {payload!r}")
        return None


def expand(config, value):
    base = config.get("~")
    if isinstance(value, str) and base is not None:
        if value.startswith("~"):
            return base + value[1:]
        if value.endswith("~"):
            return value[:-1] + base
    return value


def check_state(label, topic, payload, state_keys):
    where = f"state {label}"
    data = parse(payload, where)
    if not isinstance(data, dict):
        fail(f"{where}: not an object")
        return
    keys = list(data)
    if state_keys and keys != state_keys[0]:
        fail(f"{where}: keys {keys} differ from {state_keys[0]}")
    state_keys.append(keys)
    for key in keys:
        if key.endswith("_status"):
            value_key = key[: -len("_status")]
            status = data[key]
            if status not in STATUS_WORDS:
                fail(f"{where}: {key} is {status!r}")
            value = data.get(value_key, "missing")
            if status == "ok" and not isinstance(value, (int, float)):
                fail(f"{where}: {value_key} is {value!r} beside ok")
            if status != "ok" and value is not None:
                fail(f"{where}: {value_key} is {value!r} beside {status}")
    accuracy = data.get("iaq_accuracy", "missing")
    if accuracy is not None and accuracy not in ACCURACY_WORDS:
        fail(f"{where}: iaq_accuracy is {accuracy!r}")
    if not (data.get("rssi") is None or isinstance(data["rssi"], int)):
        fail(f"{where}: rssi is {data.get('rssi')!r}")
    if not (isinstance(data.get("uptime"), int) and data["uptime"] >= 0):
        fail(f"{where}: uptime is {data.get('uptime')!r}")


def check_config(label, topic, payload, state_topic, state_key_set, unique_ids):
    where = f"config {label} {topic}"
    levels = topic.split("/")
    if len(levels) < 5 or levels[-1] != "config" or levels[-4] != "sensor":
        fail(f"{where}: not <prefix>/sensor/<id>/<object>/config")
        return
    device_id, object_id = levels[-3], levels[-2]
    if payload == "":
        return  # the entity is removed; which ones is test_ha_discovery.c's
    if len(payload.encode("utf-8")) > CONFIG_MAX:
        fail(f"{where}: {len(payload.encode('utf-8'))} bytes")
    config = parse(payload, where)
    if not isinstance(config, dict):
        fail(f"{where}: not an object")
        return
    if expand(config, config.get("state_topic")) != state_topic:
        fail(f"{where}: state_topic expands to {expand(config, config.get('state_topic'))!r}")
    if "json_attributes_topic" in config and expand(config, config["json_attributes_topic"]) != state_topic:
        fail(f"{where}: json_attributes_topic is not the state topic")
    status_topic = state_topic[: -len("state")] + "status"
    if expand(config, config.get("availability_topic")) != status_topic:
        fail(f"{where}: availability_topic is not {status_topic}")
    for key in ("value_template", "json_attributes_template"):
        for name in re.findall(r"value_json\.([A-Za-z_][A-Za-z0-9_]*)", config.get(key, "")):
            if name not in state_key_set:
                fail(f"{where}: {key} names {name}, which the state message lacks")
    if config.get("value_template") != "{{ value_json.%s }}" % object_id:
        fail(f"{where}: value_template {config.get('value_template')!r}")
    unique_id = config.get("unique_id")
    if unique_id != f"{device_id}_{object_id}":
        fail(f"{where}: unique_id {unique_id!r}")
    if unique_id in unique_ids:
        fail(f"{where}: unique_id {unique_id!r} twice")
    unique_ids.add(unique_id)
    device = config.get("device", {})
    if device.get("identifiers") != [device_id]:
        fail(f"{where}: identifiers {device.get('identifiers')!r}")
    if not isinstance(config.get("expire_after"), int) or config["expire_after"] <= 0:
        fail(f"{where}: expire_after {config.get('expire_after')!r}")
    if "object_id" in config or "default_entity_id" in config:
        fail(f"{where}: sets the entity id")
    if config.get("device_class") == "enum" and config.get("options") != ACCURACY_WORDS:
        fail(f"{where}: options {config.get('options')!r}")
    mentions_co2 = "co2" in payload.lower() or "carbon" in payload.lower()
    if object_id == "co2":
        if config.get("device_class") != "carbon_dioxide":
            fail(f"{where}: co2 without the carbon_dioxide class")
    elif mentions_co2:
        fail(f"{where}: mentions CO2")
    if label == "escaping" and device.get("name") != ESCAPING_NAME:
        fail(f"{where}: device name came back as {device.get('name')!r}")


def main():
    if len(sys.argv) != 2:
        print("usage: mqtt_messages_check.py <mqtt_messages_dump>", file=sys.stderr)
        return 2
    run = subprocess.run([sys.argv[1]], capture_output=True, check=False)
    if run.returncode != 0:
        print(run.stderr.decode("utf-8", "replace"), file=sys.stderr)
        print(f"the dump program failed with {run.returncode}", file=sys.stderr)
        return 1
    lines = run.stdout.decode("utf-8").split("\n")
    if lines and lines[-1] == "":
        lines.pop()

    states = {}  # label -> (topic, [payload])
    configs = {}  # label -> [(topic, payload)]
    for number, line in enumerate(lines, 1):
        fields = line.split("\t")
        if len(fields) != 4 or fields[0] not in ("state", "config"):
            fail(f"line {number}: not <kind>\\t<label>\\t<topic>\\t<payload>: {line!r}")
            continue
        kind, label, topic, payload = fields
        if kind == "state":
            states.setdefault(label, (topic, []))[1].append(payload)
        else:
            configs.setdefault(label, []).append((topic, payload))

    if set(states) != LABELS or set(configs) != LABELS:
        fail(f"labels {sorted(states)} and {sorted(configs)}, expected {sorted(LABELS)}")
    state_keys = []
    for label, (topic, payloads) in sorted(states.items()):
        for payload in payloads:
            check_state(label, topic, payload, state_keys)
    state_key_set = set(state_keys[0]) if state_keys else set()
    parsed = sum(len(p) for _, p in states.values())
    for label, items in sorted(configs.items()):
        if len(items) != ENTITIES:
            fail(f"config {label}: {len(items)} entities, expected {ENTITIES}")
        unique_ids = set()
        state_topic = states.get(label, ("", []))[0]
        for topic, payload in items:
            check_config(label, topic, payload, state_topic, state_key_set, unique_ids)
            parsed += 1 if payload else 0

    for message in failures:
        print(f"FAIL {message}")
    print(f"{parsed} messages parsed, {len(failures)} failures")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
