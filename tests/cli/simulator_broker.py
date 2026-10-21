#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The simulator in front of a real broker: architecture section 10's milestone
for phase 4, short of a Home Assistant (docs/adr/0030-telemetry-ports-and-the-
reporter.md, amendment "The simulator on a real broker").

Against a private mosquitto on a free port of 127.0.0.1, with the simulator
headless at --time-scale 60:

  * it appears: "online", every discovery config and the state, all retained
    -- what Home Assistant reads to make the device and its entities;
  * the state keeps coming: the heartbeat and the deltas, live;
  * Home Assistant restarting -- its birth, "online" on homeassistant/status,
    live -- brings all nine configs again;
  * the broker restarting, with nothing kept: the session drops, the
    simulator connects again on its own and announces itself again;
  * the simulator killed (SIGKILL): the broker publishes its will, "offline",
    retained -- Home Assistant shows the device unavailable;
  * a run that ends by itself (--duration) ends the same way;
  * a broker that wants a password: the wrong one is refused and the log says
    so, the right one -- from HAC_MQTT_PASSWORD -- logs in, and the password is
    printed nowhere;
  * what the simulator refuses before it starts: a board's id, a time scale a
    real broker cannot keep up with, a password in the URL.

A small MQTT 3.1.1 client of its own watches the broker, so that nothing but
the broker itself needs installing. Without a broker, or with a simulator
built without libmosquitto, it says it was not exercised and exits 77, which
CTest reports as skipped.

  simulator_broker.py --simulator <path> --mosquitto <path> [--mosquitto-passwd <path>]
                      --scenarios <dir> --work <dir> [--with-mqtt]
"""

import argparse
import json
import os
import signal
import socket
import struct
import subprocess
import sys
import time

SKIPPED = 77
DEVICE = "sim-cli"
STATUS = f"homeair/{DEVICE}/status"
STATE = f"homeair/{DEVICE}/state"
CONFIGS = f"homeassistant/sensor/{DEVICE}/+/config"
HA_STATUS = "homeassistant/status"
# Every entity ha_discovery.c announces; the two the scenarios cannot provide
# (no iaq column) are announced empty, which removes them.
ENTITIES = ["temperature", "humidity", "pressure", "co2", "iaq", "gas_resistance",
            "iaq_accuracy", "rssi", "uptime"]
PROVIDED = ["temperature", "humidity", "pressure", "co2", "gas_resistance", "rssi", "uptime"]
PASSWORD = "not-this-one-7f3c"  # what HAC_MQTT_PASSWORD carries in the login check


def fail(message):
    print(f"FAIL: {message}")
    sys.exit(1)


def note(message):
    print(f"[test] {message}", flush=True)


# --- a minimal MQTT 3.1.1 client: CONNECT, SUBSCRIBE at QoS 0, PUBLISH at QoS 0 ----------

def utf8(text):
    data = text.encode()
    return struct.pack(">H", len(data)) + data


def remaining_length(n):
    out = bytearray()
    while True:
        byte = n % 128
        n //= 128
        out.append(byte | (0x80 if n else 0))
        if not n:
            return bytes(out)


class Client:
    def __init__(self, port, client_id):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.buf = b""
        self.pending = []
        self.send(0x10, utf8("MQTT") + bytes([4, 0x02]) + struct.pack(">H", 60) + utf8(client_id))
        kind, body = self.read(5.0)
        if kind != 0x20 or len(body) < 2 or body[1] != 0:
            raise RuntimeError(f"CONNACK refused or missing: {kind} {body!r}")

    def send(self, header, body):
        self.sock.sendall(bytes([header]) + remaining_length(len(body)) + body)

    def read(self, timeout):
        """One packet, (first byte, body), or (None, None) when the time is up."""
        deadline = time.monotonic() + timeout
        while True:
            packet = self.parse()
            if packet is not None:
                return packet
            left = deadline - time.monotonic()
            if left <= 0:
                return None, None
            self.sock.settimeout(left)
            try:
                data = self.sock.recv(65536)
            except socket.timeout:
                return None, None
            if not data:
                raise ConnectionError("the broker closed the connection")
            self.buf += data

    def parse(self):
        if len(self.buf) < 2:
            return None
        n, mult, i = 0, 1, 1
        while True:
            if i >= len(self.buf):
                return None
            byte = self.buf[i]
            n += (byte & 0x7F) * mult
            mult *= 128
            i += 1
            if not byte & 0x80:
                break
        if len(self.buf) < i + n:
            return None
        first, body = self.buf[0], self.buf[i:i + n]
        self.buf = self.buf[i + n:]
        return first, body

    def subscribe(self, *filters):
        body = struct.pack(">H", 1) + b"".join(utf8(f) + b"\x00" for f in filters)
        self.send(0x82, body)
        while True:
            kind, body = self.read(5.0)
            if kind is None:
                raise RuntimeError("no SUBACK")
            if kind == 0x90:
                return
            if kind & 0xF0 == 0x30:
                self.pending.append(self.decode(kind, body))

    def publish(self, topic, payload, retain=False):
        self.send(0x30 | (1 if retain else 0), utf8(topic) + payload.encode())

    @staticmethod
    def decode(first, body):
        n = struct.unpack(">H", body[:2])[0]
        topic = body[2:2 + n].decode()
        rest = body[2 + n:]
        if (first >> 1) & 0x03:  # QoS 1 or 2: a packet id first
            rest = rest[2:]
        return topic, rest.decode(errors="replace"), bool(first & 0x01)

    def messages(self, seconds):
        """Every PUBLISH within `seconds`, as (topic, payload, retained)."""
        out, self.pending = self.pending, []
        deadline = time.monotonic() + seconds
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                return out
            kind, body = self.read(left)
            if kind is None:
                return out
            if kind & 0xF0 == 0x30:
                out.append(self.decode(kind, body))

    def close(self):
        try:
            self.send(0xE0, b"")
        finally:
            self.sock.close()


_watchers = 0


def retained(port, filters, settle=0.4):
    """What the broker keeps under `filters` now: {topic: payload}."""
    global _watchers
    _watchers += 1
    c = Client(port, f"hac-test-watch-{os.getpid()}-{_watchers}")
    c.subscribe(*filters)
    kept = {t: p for t, p, r in c.messages(settle) if r}
    c.close()
    return kept


def wait_for(what, predicate, port, filters, within):
    deadline = time.monotonic() + within
    kept = {}
    while time.monotonic() < deadline:
        kept = retained(port, filters)
        if predicate(kept):
            return kept
        time.sleep(0.2)
    fail(f"{what} within {within} s; the broker keeps: {sorted(kept)}")


# --- the processes ------------------------------------------------------------------------

def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Broker:
    def __init__(self, binary, port, work):
        self.binary, self.port, self.work = binary, port, work
        self.proc = None
        self.runs = 0

    def start(self, extra=""):
        self.runs += 1
        conf = os.path.join(self.work, f"broker-{self.runs}.conf")
        log = os.path.join(self.work, f"broker-{self.runs}.log")
        with open(conf, "w") as f:
            f.write(f"listener {self.port} 127.0.0.1\n"
                    "persistence false\n"
                    f"log_dest file {log}\nlog_type all\n" +
                    (extra or "allow_anonymous true\n"))
        self.proc = subprocess.Popen([self.binary, "-c", conf])
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                fail(f"the broker exited at once; its log is {log}")
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=0.5).close()
                return
            except OSError:
                time.sleep(0.05)
        fail(f"the broker did not listen on {self.port}; its log is {log}")

    def stop(self):
        if self.proc is not None and self.proc.poll() is None:
            self.proc.terminate()
            self.proc.wait(timeout=5)
        self.proc = None


class Sim:
    def __init__(self, args, port, name, extra, env=None):
        self.out_path = os.path.join(args.work, f"{name}.txt")
        self.out = open(self.out_path, "w")
        cmd = [args.simulator, "--headless", "--time-scale", "60",
               "--wall-time", "2026-10-20T12:00:00Z",
               "--scenario-co2", os.path.join(args.scenarios, "co2_spike_meeting.csv"),
               "--scenario-env", os.path.join(args.scenarios, "normal_day.csv"),
               "--broker", f"mqtt://127.0.0.1:{port}", "--device-id", DEVICE] + extra
        full_env = dict(os.environ)
        full_env.pop("HAC_MQTT_PASSWORD", None)
        full_env.update(env or {})
        self.proc = subprocess.Popen(cmd, stdout=self.out, stderr=subprocess.STDOUT, env=full_env)

    def wait(self, timeout):
        try:
            code = self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            fail(f"the simulator did not end within {timeout} s; see {self.out_path}")
        self.out.close()
        return code

    def kill(self, sig=signal.SIGKILL):
        if self.proc.poll() is None:
            self.proc.send_signal(sig)
            self.proc.wait(timeout=10)
        self.out.close()

    def output(self):
        with open(self.out_path) as f:
            return f.read()


def refused(args, extra, env=None):
    """Runs the simulator with arguments it must refuse; its output."""
    cmd = [args.simulator, "--headless", "--duration", "10",
           "--scenario-co2", os.path.join(args.scenarios, "co2_spike_meeting.csv")] + extra
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=30,
                            env={**os.environ, **(env or {})})
    if result.returncode != 2:
        fail(f"{extra} was not refused (exit {result.returncode}):\n{result.stdout}{result.stderr}")
    return result.stdout + result.stderr


# --- the checks ----------------------------------------------------------------------------

def appears(port):
    """Everything Home Assistant reads to build the device, retained."""
    kept = wait_for("the simulator announced itself", lambda k: k.get(STATUS) == "online" and
                    STATE in k and all(f"homeassistant/sensor/{DEVICE}/{e}/config" in k
                                       for e in PROVIDED),
                    port, [STATUS, STATE, CONFIGS], within=15)
    for entity in PROVIDED:
        config = json.loads(kept[f"homeassistant/sensor/{DEVICE}/{entity}/config"])
        if config.get("unique_id") != f"{DEVICE}_{entity}" or config.get("~") != f"homeair/{DEVICE}":
            fail(f"{entity}'s config is not this device's: {config}")
    for entity in set(ENTITIES) - set(PROVIDED):
        if f"homeassistant/sensor/{DEVICE}/{entity}/config" in kept:
            fail(f"{entity} is announced, but the scenarios do not provide it")
    state = json.loads(kept[STATE])
    if not isinstance(state.get("co2"), (int, float)) or state.get("co2_status") != "ok":
        fail(f"the retained state has no CO2: {state}")
    return kept


def the_state_keeps_coming(port):
    c = Client(port, f"hac-test-live-{os.getpid()}")
    c.subscribe(STATE)
    live = [m for m in c.messages(4.0) if m[0] == STATE and not m[2]]
    c.close()
    # x60: a heartbeat a second at least, and the CO2 spike's deltas.
    if len(live) < 3:
        fail(f"{len(live)} live state messages in 4 s at x60, not at least 3")
    note(f"{len(live)} live state messages in 4 s")


def home_assistant_restarts(port):
    c = Client(port, f"hac-test-ha-{os.getpid()}")
    c.subscribe(CONFIGS)
    c.messages(0.4)  # what is kept
    c.publish(HA_STATUS, "online", retain=False)
    live = {t for t, p, r in c.messages(4.0) if not r}
    c.close()
    expected = {f"homeassistant/sensor/{DEVICE}/{e}/config" for e in ENTITIES}
    if live != expected:
        fail(f"Home Assistant's birth brought {sorted(live)}, not all nine configs")
    note("Home Assistant's birth brought all nine configs again")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--simulator", required=True)
    parser.add_argument("--mosquitto", default="")
    parser.add_argument("--mosquitto-passwd", default="")
    parser.add_argument("--scenarios", required=True)
    parser.add_argument("--work", required=True)
    parser.add_argument("--with-mqtt", action="store_true")
    args = parser.parse_args()

    if not args.with_mqtt:
        print("not exercised: the simulator was built without libmosquitto, so it has no --broker")
        return SKIPPED
    if not args.mosquitto or not os.access(args.mosquitto, os.X_OK):
        print("not exercised: no mosquitto broker was found when this build was configured")
        return SKIPPED
    os.makedirs(args.work, exist_ok=True)

    # What is refused before anything runs.
    refused(args, ["--broker", "mqtt://127.0.0.1:1", "--device-id", "hac-a1b2c3d4e5f6"])
    refused(args, ["--broker", "mqtt://127.0.0.1:1", "--time-scale", "3600"])
    out = refused(args, ["--broker", f"mqtt://hac:{PASSWORD}@127.0.0.1:1"])
    if PASSWORD in out:
        fail(f"a password in the URL was printed:\n{out}")
    refused(args, ["--mqtt-user", "hac"])
    refused(args, ["--broker", "mqtt://127.0.0.1:1", "--telemetry", "loopback"])
    note("a board's id, x3600, a password in the URL, a user without a broker: refused")

    port = free_port()
    broker = Broker(args.mosquitto, port, args.work)
    sims = []
    try:
        broker.start()
        sim = Sim(args, port, "sim-first", ["--duration", "0"])
        sims.append(sim)
        appears(port)
        note("it appears: online, seven configs and the state, retained; two configs empty")
        the_state_keeps_coming(port)
        home_assistant_restarts(port)

        broker.stop()
        time.sleep(1.0)
        broker.start()
        appears(port)
        note("the broker restarted with nothing kept: the simulator announced itself again")

        sim.kill(signal.SIGKILL)
        wait_for("the will after SIGKILL", lambda k: k.get(STATUS) == "offline", port, [STATUS],
                 within=5)
        out = sim.output()
        for line in ("session up session=1", "session up session=2",
                     "[publish] online, 9 discovery configs session=2"):
            if line not in out:
                fail(f"the first run's output has no \"{line}\":\n{out}")
        note("killed: the broker published the will, offline, retained")

        sim = Sim(args, port, "sim-duration", ["--duration", "120"])
        sims.append(sim)
        code = sim.wait(timeout=30)
        out = sim.output()
        if code != 0 or "the session ends without a DISCONNECT" not in out:
            fail(f"the run with --duration ended with {code}:\n{out}")
        wait_for("the will after --duration", lambda k: k.get(STATUS) == "offline", port,
                 [STATUS], within=5)
        note("--duration ran out: the will again")

        if args.mosquitto_passwd and os.access(args.mosquitto_passwd, os.X_OK):
            passwd = os.path.join(args.work, "passwd")
            if os.path.exists(passwd):
                os.remove(passwd)
            subprocess.run([args.mosquitto_passwd, "-c", "-b", passwd, "hac", PASSWORD],
                           check=True, capture_output=True)
            os.chmod(passwd, 0o600)
            broker.stop()
            broker.start(f"allow_anonymous false\npassword_file {passwd}\n")

            sim = Sim(args, port, "sim-wrong-password", ["--duration", "120", "--mqtt-user", "hac"],
                      env={"HAC_MQTT_PASSWORD": "wrong"})
            sims.append(sim)
            sim.wait(timeout=30)
            out = sim.output()
            if "not authorised" not in out or "session up" in out:
                fail(f"the wrong password was not refused, or not said so:\n{out}")
            sim = Sim(args, port, "sim-password", ["--duration", "120", "--mqtt-user", "hac"],
                      env={"HAC_MQTT_PASSWORD": PASSWORD})
            sims.append(sim)
            sim.wait(timeout=30)
            out = sim.output()
            if "session up session=1" not in out or PASSWORD in out:
                fail(f"the right password did not log in, or was printed:\n{out}")
            note("a broker with a password: the wrong one refused and logged, the right one in, "
                 "neither printed")
        else:
            print("not exercised: a broker with a password -- mosquitto_passwd was not found")
    finally:
        for s in sims:
            if s.proc.poll() is None:
                s.proc.kill()
        broker.stop()
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
