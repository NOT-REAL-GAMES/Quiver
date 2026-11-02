#!/usr/bin/env python3
"""Exercise opt-in NPC paging, reference pinning, and checkpoint/page ownership.

An original generated classic-v6 QC program creates a distant streamable actor
with self references and owned registered position aliases. An actual UDP player
moves its interest by console teleport while the fixture verifies absence,
restoration, canonical position, and uniqueness. Additional NPCs exercise legacy,
incoming-reference, outgoing-reference, and external-handle pinning. Temporary
assets/configuration/databases are isolated from the supplied game directory.
"""

import argparse
import json
import os
from pathlib import Path
import pty
import re
import selectors
import shutil
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "qc"))
from generate_checkpoint_fixture import Program
from run_world_network_smoke import Client, copied_engine_environment


DORMANT = "Paging fixture dormant PASS"
AWAKE = "Paging fixture awake PASS"
CHURNED = "Paging fixture recycled slot PASS"
FAR_COMMAND = "world_teleport 1024 -1024 0 128 128 512"
# The actor is within 2048 units, although its chunk center is farther away.
# Loading must use its exact canonical position, not the chunk-center distance.
EDGE_COMMAND = "world_teleport 1022 -1024 0 200 128 512"
NEAR_COMMAND = "world_teleport 0 0 0 128 128 256"


def generate(root):
    p = Program(root)
    for name in ("main", "PlayerPreThink", "PlayerPostThink", "ClientKill", "ClientConnect", "PutClientInServer",
                 "ClientDisconnect", "SetNewParms", "SetChangeParms", "info_player_start", "info_player_deathmatch"):
        p.function(name)
    for name in ("wp_register_field", "wp_register_global", "wp_alloc", "wp_free", "wp_valid", "wp_add",
                 "wp_setentity", "wp_getentity", "wp_distance", "wp_streamable"):
        p.function(name, 0)
    for name, number in (("error", 10), ("spawn", 14), ("find", 18), ("dprint", 25)):
        p.function(name, -number)

    def field(name, datatype, width=1):
        offset = p.entityfields
        p.entityfields += width
        p.fielddefs.append((datatype, offset, p.string(name)))
        return p.global_value(name, 5, offset)

    home = field("paging_home", 3, 3)
    alias = field("paging_alias", 3, 3)
    self_field = field("paging_self", 4)
    marker = field("paging_value_x", 2)  # A real scalar ending _x must survive.
    classname = p.global_offsets["classname"]
    owner = p.global_offsets["owner"]
    world = p.global_offsets["world"]
    actor = p.global_value("paging_actor_temporary", 4)
    other = p.global_value("paging_other_temporary", 4)
    pinned = p.global_value("paging_pinned_reference", 4, persistent=True)
    pointer = p.global_value("paging_pointer_temporary", 7)
    handle = p.vector("paging_handle_temporary")
    probe = p.vector("paging_probe_temporary")
    scratch = p.vector("paging_scratch_temporary")
    external = p.vector("paging_external_alias", persistent=True)
    present = p.global_value("paging_present", 2, 0x3f800000, persistent=True)
    zero = p.global_value("paging_zero", 2)
    one = p.global_value("paging_one", 2, 0x3f800000)
    value = p.global_value("paging_value", 2, 0x42140000)  # 37.0
    zeros = p.vector("paging_zero_vector")
    offset = p.vector("paging_far_offset", (1048576.125, -1048575.75, 512.5))
    failure = p.text("paging_failure", "entity paging fixture assertion failed")
    dormant = p.text("paging_dormant_message", DORMANT + "\n")
    awake = p.text("paging_awake_message", AWAKE + "\n")
    churn_message = p.text("paging_churn_message", CHURNED + "\n")
    names = {name: p.text("paging_name_" + name, "paging_" + name)
             for name in ("actor", "incoming", "legacy", "outgoing", "handle", "churn")}
    field_names = [p.text("paging_registration_" + name, name) for name in ("paging_home", "paging_alias")]
    external_name = p.text("paging_external_registration", "paging_external_alias")
    churned = p.global_value("paging_churned", 2, persistent=True)
    churn_time = p.global_value("paging_churn_time", 2, persistent=True)
    reuse_delay = p.vector("paging_reuse_delay", (2.25, 0, 0))
    mutation_end = p.global_value("paging_mutation_end", 2, 0x40c00000, persistent=True)  # 6 seconds.
    mutating = p.global_value("paging_mutating", 2, 0x3f800000, persistent=True)
    expected = p.global_value("paging_expected_temporary", 2)

    def patch(jump):
        op, a, _, c = p.statements[jump]
        p.statements[jump] = (op, a, len(p.statements) - jump, c)

    def store_field(subject, field_offset, source, kind="F"):
        p.op("ADDRESS", subject, field_offset, pointer)
        p.op("STOREP_" + kind, source, pointer)
        p.op("STORE_F", zero, pointer)

    def clear_temporaries():
        for temporary in (actor, other, pointer):
            p.op("STORE_F", zero, temporary)
        for temporary in (handle, probe, scratch):
            p.op("STORE_V", zeros, temporary)

    def create(name, streamable, owned_handles=False):
        p.call("spawn"); p.op("STORE_ENT", 1, actor)
        store_field(actor, classname, names[name], "S")
        p.call("wp_alloc", (actor, False)); p.op("STORE_V", 1, handle)
        p.call("wp_add", (handle, True), (handle, True), (offset, True))
        p.call("wp_setentity", (actor, False), (handle, True))
        if owned_handles:
            store_field(actor, home, handle, "V")
            store_field(actor, alias, handle, "V")
        else:
            p.call("wp_free", (handle, True))
        if streamable:
            p.call("wp_streamable", (actor, False), (one, False)); p.assert_true(1, failure)

    def find(name, target=actor, start=None):
        p.call("find", (world if start is None else start, False), (classname, False), (names[name], False))
        p.op("STORE_ENT", 1, target)

    p.function("worldspawn", len(p.statements))
    for name in field_names:
        p.call("wp_register_field", (name, False))
    p.call("wp_register_global", (external_name, False))
    create("actor", True, True)
    store_field(actor, self_field, actor, "ENT")
    store_field(actor, marker, value)
    create("incoming", True)
    p.op("STORE_ENT", actor, pinned)
    create("legacy", False)
    create("outgoing", True)
    store_field(actor, owner, pinned, "ENT")
    create("handle", True, True)
    p.op("STORE_V", handle, external)
    clear_temporaries()
    p.op("DONE")

    p.function("StartFrame", len(p.statements))
    for name in ("incoming", "legacy", "outgoing", "handle"):
        find(name)
        p.op("NE_E", actor, world, scratch); p.assert_true(scratch, failure)
    find("actor")
    absent = p.op("IFNOT", actor)
    # Exactly one live instance; self and owned handle aliases must be remapped
    # to the new slot/generations rather than preserving raw old VM addresses.
    find("actor", other, actor)
    p.op("NOT_ENT", other, 0, scratch); p.assert_true(scratch, failure)
    p.op("LOAD_ENT", actor, self_field, other)
    p.op("EQ_E", actor, other, scratch); p.assert_true(scratch, failure)
    # Paging queues before StartFrame. A changing scalar must invalidate each
    # pending snapshot until this finite mutation window has finished.
    p.op("STORE_F", value, expected)
    p.op("LT", p.global_offsets["time"], mutation_end, scratch)
    stop_mutating = p.op("IFNOT", scratch)
    p.op("STORE_F", p.global_offsets["time"], expected)
    store_field(actor, marker, expected)
    mutation_done = p.op("GOTO")
    patch(stop_mutating)
    already_stable = p.op("IFNOT", mutating)
    store_field(actor, marker, value)
    p.op("STORE_F", zero, mutating)
    patch(already_stable)
    op, _, b, c = p.statements[mutation_done]
    p.statements[mutation_done] = (op, len(p.statements) - mutation_done, b, c)
    p.op("LOAD_F", actor, marker, scratch)
    p.op("EQ_F", scratch, expected, scratch); p.assert_true(scratch, failure)
    p.op("LOAD_V", actor, home, handle)
    p.call("wp_valid", (handle, True)); p.assert_true(1, failure)
    p.op("LOAD_V", actor, alias, scratch)
    p.op("EQ_V", handle, scratch, scratch); p.assert_true(scratch, failure)
    p.call("wp_alloc", (world, False)); p.op("STORE_V", 1, probe)
    p.call("wp_getentity", (probe, True), (actor, False))
    p.call("wp_distance", (handle, True), (probe, True))
    p.op("EQ_F", 1, zero, scratch); p.assert_true(scratch, failure)
    p.call("wp_free", (probe, True))
    already_present = p.op("IF", present)
    p.call("dprint", (awake, False))
    patch(already_present)
    p.op("STORE_F", one, present)
    end = p.op("GOTO")
    patch(absent)
    p.op("NOT_F", mutating, 0, scratch); p.assert_true(scratch, failure)
    already_absent = p.op("IFNOT", present)
    p.call("dprint", (dormant, False))
    p.op("ADD_F", p.global_offsets["time"], reuse_delay, churn_time)
    patch(already_absent)
    p.op("STORE_F", zero, present)
    # Wait past the engine's free-edict reuse delay, then occupy the old slot.
    # Restoration must remap self references and position handle owners.
    skip_churn = p.op("IF", churned)
    p.op("GE", p.global_offsets["time"], churn_time, scratch)
    wait_churn = p.op("IFNOT", scratch)
    for _ in range(8):
        p.call("spawn"); p.op("STORE_ENT", 1, other)
        store_field(other, classname, names["churn"], "S")
    p.op("STORE_F", one, churned)
    p.call("dprint", (churn_message, False))
    patch(wait_churn)
    patch(skip_churn)
    op, _, b, c = p.statements[end]
    p.statements[end] = (op, len(p.statements) - end, b, c)
    clear_temporaries()
    p.op("DONE")
    return p.encode()


def run(binary, work, phase, timeout):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    master, slave = pty.openpty()
    os.set_blocking(master, False)
    command = [str(binary), "-basedir", str(work), "-userdir", str(work), "-game", "paging_fixture",
               "-dedicated", "1", "-port", str(port), "+developer", "1"]
    command += ["+world_create", "paging_fixture", "empty", "1"] if phase == "unsaved" else ["+world_load", "paging_fixture"]
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=slave, stderr=slave, cwd=work)
    os.close(slave)
    selector = selectors.DefaultSelector()
    selector.register(master, selectors.EVENT_READ, "console")
    client = Client(0, ("127.0.0.1", port), selector)
    output = bytearray()
    started = time.monotonic()
    restoring = False

    def pump():
        now = time.monotonic()
        client.pump(now, started, False, False)
        for key, _ in selector.select(0.02):
            if key.data == "console":
                try:
                    output.extend(os.read(master, 65536))
                except OSError:
                    pass
            else:
                while True:
                    try:
                        data, address = client.socket.recvfrom(65536)
                    except BlockingIOError:
                        break
                    try:
                        client.receive(data, address, time.monotonic(), started)
                    except RuntimeError as error:
                        if not restoring or "disconnected" not in str(error):
                            raise

    def wait_for(text=None, occurrence=1):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if (text is None and client.frames) or (text is not None and output.count(text.encode()) >= occurrence):
                return
            if process.poll() is not None:
                raise RuntimeError(f"server exited {process.returncode} before {text!r}")
            pump()
        raise RuntimeError(f"timeout waiting for {text!r}")

    def send(text):
        process.stdin.write((text + "\n").encode())
        process.stdin.flush()

    failure = None
    try:
        wait_for()
        if phase == "unsaved":
            wait_for(DORMANT)
        elif phase == "initial":
            wait_for(DORMANT)
            wait_for(CHURNED)
            send("world_save")
            wait_for("World saved")
            send("world_snapshot create dormant")
            wait_for("World snapshot create: dormant")
            send(EDGE_COMMAND)
            wait_for(AWAKE)
            send(NEAR_COMMAND)
            wait_for(DORMANT, 2)
        elif phase == "dormant_reload":
            wait_for("World checkpoint restored:")
            send(FAR_COMMAND)
            wait_for(AWAKE)
            send("world_save")
            wait_for("World saved")
        elif phase == "named_restore":
            wait_for("World checkpoint restored:")
            restoring = True
            send("world_snapshot restore dormant")
            wait_for("World checkpoint restored:", 2)
            selector.unregister(client.socket)
            client.socket.close()
            client = Client(0, ("127.0.0.1", port), selector)
            restoring = False
            wait_for()
            send(FAR_COMMAND)
            wait_for(AWAKE)
        else:
            wait_for("World checkpoint restored:")
            send(NEAR_COMMAND)
            wait_for(DORMANT)
            send(FAR_COMMAND)
            wait_for(AWAKE)
        send("world_stats;quit")
        deadline = time.monotonic() + 10
        while process.poll() is None and time.monotonic() < deadline:
            # Continue transport ACKs while the server shuts the connection down.
            try:
                pump()
            except RuntimeError as error:
                if "disconnected" not in str(error):
                    raise
        process.wait(timeout=2)
    except Exception as error:
        failure = str(error)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        while True:
            try:
                data = os.read(master, 65536)
                if not data:
                    break
                output.extend(data)
            except (OSError, BlockingIOError):
                break
        client.socket.close()
        selector.close()
        os.close(master)
        log = work / f"{phase}.log"
        log.write_bytes(output)
    if failure or process.returncode or b"assertion failed" in output:
        raise RuntimeError(f"{failure or 'fixture/server failed'}; log: {log}\n" + output.decode(errors="replace")[-6000:])
    evicted_slots = re.findall(rb"Entity page (\d+) evicted from slot (\d+)", output)
    loaded_slots = re.findall(rb"Entity page (\d+) loaded in slot (\d+)", output)
    cancellations = re.findall(rb"(\d+) cancelled evictions", output)
    if phase in ("unsaved", "initial") and (not cancellations or not int(cancellations[-1])):
        raise RuntimeError(f"fixture did not exercise cancelled asynchronous snapshots; log: {log}")
    if phase == "initial" and (not evicted_slots or not loaded_slots or evicted_slots[0][1] == loaded_slots[0][1]):
        raise RuntimeError(f"fixture did not exercise a different restored edict slot; log: {log}")
    return {"phase": phase, "log": str(log), "frames": client.frames,
            "evicted_slots": [[int(identity), int(slot)] for identity, slot in evicted_slots],
            "loaded_slots": [[int(identity), int(slot)] for identity, slot in loaded_slots],
            "cancelled_evictions": int(cancellations[-1]) if cancellations else 0,
            "exact_radius_edge_check": phase == "active_reload",
            "dormant_assertions": output.count(DORMANT.encode()), "awake_assertions": output.count(AWAKE.encode())}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--basedir", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=30)
    args = parser.parse_args()
    os.environ.update(copied_engine_environment(args.binary))
    source = args.basedir.resolve()
    if (source / "id1").is_dir():
        source /= "id1"
    assets = sorted(source.glob("pak*.pak"))
    if not assets:
        parser.error("basedir contains no pak*.pak assets")
    work = Path(tempfile.mkdtemp(prefix="quiver-paging-smoke-"))
    (work / "id1").mkdir()
    for asset in assets:
        (work / "id1" / asset.name).symlink_to(asset.resolve())
    (work / "paging_fixture").mkdir()
    (work / "paging_fixture/progs.dat").write_bytes(generate(ROOT))
    binary = work / "quiver-server"
    shutil.copy2(args.binary.resolve(), binary)
    summary = {"kind": "opt-in self-contained NPC paging and checkpoint smoke", "success": False, "work": str(work), "runs": []}
    try:
        for phase in ("unsaved", "initial", "dormant_reload", "active_reload", "named_restore"):
            result = run(binary, work, phase, args.timeout)
            summary["runs"].append(result)
            databases = list(work.rglob("paging_fixture.sqlite3"))
            if len(databases) != 1:
                raise RuntimeError("expected one isolated world database")
            with sqlite3.connect(databases[0]) as db:
                if db.execute("PRAGMA integrity_check").fetchone() != ("ok",):
                    raise RuntimeError("paging database failed integrity_check")
                heads = db.execute("SELECT hex(h.id),h.revision,v.deleted FROM entity_page_heads h "
                                   "JOIN entity_page_versions v ON h.id=v.id AND h.revision=v.revision").fetchall()
                checkpoint = db.execute("SELECT hex(id),revision FROM entity_page_checkpoint").fetchall()
                result["page_heads"] = heads
                result["checkpoint_pages"] = checkpoint
                if len(heads) != 1 or heads[0][2] or len(checkpoint) != (0 if phase == "unsaved" else 1):
                    raise RuntimeError("expected exactly one durable actor page and checkpoint page")
                if phase == "unsaved":
                    continue
                if phase == "initial":
                    if heads[0][1] <= checkpoint[0][1]:
                        raise RuntimeError("fixture did not create a newer page after the checkpoint")
                    if heads[0][0] == summary["runs"][0]["page_heads"][0][0]:
                        raise RuntimeError("unsaved page survived the fresh worldspawn")
                elif heads[0][0] != summary["runs"][1]["page_heads"][0][0]:
                    raise RuntimeError("actor stable identity changed across a cold restart")
                if phase == "dormant_reload" and heads[0][1] != summary["runs"][1]["checkpoint_pages"][0][1]:
                    raise RuntimeError("cold reload did not restore the checkpoint page manifest")
                if phase == "active_reload" and heads[0][1] <= summary["runs"][1]["page_heads"][0][1]:
                    raise RuntimeError("page revision clock was reused after checkpoint rollback")
        summary["database"] = str(databases[0])
        summary["success"] = True
    finally:
        (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, indent=2), flush=True)


if __name__ == "__main__":
    main()
