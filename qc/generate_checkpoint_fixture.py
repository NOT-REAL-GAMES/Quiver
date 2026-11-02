#!/usr/bin/env python3
"""Emit a tiny classic-v6 progs.dat that exercises the world-position checkpoint API.

This is an original deterministic test program, not a QuakeC compiler or a copy of
the base game. It uses the engine's checked-in ABI/opcode declarations and needs
only Python. Run a dedicated server with the resulting progs.dat and developer 1;
world_create/world_save/world_load exercise its per-frame assertions. The matching
checkpoint_fixture.qc describes the emitted program in readable QuakeC.
"""

import argparse
from pathlib import Path
import re
import struct


class Program:
    def __init__(self, root):
        self.strings = bytearray(b"\0")
        self.string_offsets = {"": 0}
        self.globals = []
        self.globaldefs = [(0, 0, 0)]
        self.fielddefs = [(0, 0, 0)]
        self.global_offsets = {}
        self.field_offsets = {}
        self.functions = [(0, 0, 0, 0, 0, 0, 0, b"\0" * 8)]
        opcode_text = (root / "Quake/pr_comp.h").read_text()
        names = re.findall(r"\bOP_[A-Z0-9_]+\b", opcode_text.split("typedef enum opcode_t")[1].split("} opcode_t")[0])
        self.ops = {name: index for index, name in enumerate(names)}
        self.statements = [(self.ops["OP_DONE"], 0, 0, 0)] * 2
        native = (root / "Quake/progdefs.q1").read_text()
        for text, kind in re.findall(r"typedef struct\s*\{(.*?)\}\s*(globalvars_t|entvars_t);", native, re.S):
            offset = 0
            for typename, name, count in re.findall(r"\b(int|float|string_t|vec3_t|func_t)\s+(\w+)(?:\[(\d+)\])?;", text):
                width = int(count) if count else 3 if typename == "vec3_t" else 1
                datatype = {"int": 4, "float": 2, "string_t": 1, "vec3_t": 3, "func_t": 6}[typename]
                if kind == "globalvars_t":
                    self.globals.extend([0] * width)
                    if name != "pad":
                        self.globaldefs.append((datatype, offset, self.string(name)))
                        self.global_offsets[name] = offset
                else:
                    self.fielddefs.append((datatype, offset, self.string(name)))
                    self.field_offsets[name] = offset
                offset += width
            if kind == "entvars_t":
                self.entityfields = offset
        for name, offset in self.field_offsets.items():
            self.global_value(name, 5, offset)

    def string(self, value):
        if value not in self.string_offsets:
            self.string_offsets[value] = len(self.strings)
            self.strings.extend(value.encode("ascii") + b"\0")
        return self.string_offsets[value]

    def global_value(self, name, datatype, value=0, persistent=False):
        offset = len(self.globals)
        self.global_offsets[name] = offset
        words = value if isinstance(value, list) else [value]
        self.globals.extend(words)
        self.globaldefs.append((datatype | (0x8000 if persistent else 0), offset, self.string(name)))
        return offset

    def vector(self, name, values=(0, 0, 0), persistent=False):
        return self.global_value(name, 3, [struct.unpack("<I", struct.pack("<f", value))[0] for value in values], persistent)

    def text(self, name, value):
        return self.global_value(name, 1, self.string(value))

    def function(self, name, statement=1):
        index = len(self.functions)
        self.functions.append((statement, 0, 0, 0, self.string(name), self.string("checkpoint_fixture.qc"), 0, b"\0" * 8))
        if name in self.global_offsets:
            self.globals[self.global_offsets[name]] = index
        else:
            self.global_value(name, 6, index)
        return self.global_offsets[name]

    def op(self, name, a=0, b=0, c=0):
        self.statements.append((self.ops["OP_" + name], a, b, c))
        return len(self.statements) - 1

    def call(self, name, *parameters):
        for index, (offset, vector) in enumerate(parameters):
            self.op("STORE_V" if vector else "STORE_F", offset, 4 + index * 3)
        self.op("CALL" + str(len(parameters)), self.global_offsets[name])

    def assert_true(self, value, message):
        jump = self.op("IF", value)
        self.call("error", (message, False))
        op, a, _, c = self.statements[jump]
        self.statements[jump] = (op, a, len(self.statements) - jump, c)

    def encode(self):
        chunks = [b"".join(struct.pack("<Hhhh", *entry) for entry in self.statements),
                  b"".join(struct.pack("<HHi", *entry) for entry in self.globaldefs),
                  b"".join(struct.pack("<HHi", *entry) for entry in self.fielddefs),
                  b"".join(struct.pack("<7i8s", *entry) for entry in self.functions),
                  bytes(self.strings), struct.pack("<" + "I" * len(self.globals), *self.globals)]
        counts = [len(self.statements), len(self.globaldefs), len(self.fielddefs), len(self.functions), len(self.strings), len(self.globals)]
        header = [6, 5927]
        cursor = 60
        for chunk, count in zip(chunks, counts):
            header.extend((cursor, count))
            cursor += len(chunk)
        header.append(self.entityfields)
        return struct.pack("<15i", *header) + b"".join(chunks)


def generate(root):
    p = Program(root)
    for name in ("main", "PlayerPreThink", "PlayerPostThink", "ClientKill", "ClientConnect", "PutClientInServer",
                 "ClientDisconnect", "SetNewParms", "SetChangeParms", "info_player_start", "info_player_deathmatch"):
        p.function(name)
    for name in ("wp_register_field", "wp_register_global", "wp_alloc", "wp_valid", "wp_add", "wp_setentity", "wp_getentity", "wp_distance"):
        p.function(name, 0)
    for name, number in (("error", 10), ("spawn", 14), ("dprint", 25)):
        p.function(name, -number)
    home_field = p.entityfields
    p.entityfields += 3
    p.fielddefs.append((3, home_field, p.string("checkpoint_home")))
    home = p.global_value("checkpoint_home", 5, home_field)
    anchor = p.vector("checkpoint_anchor", persistent=True)
    alias = p.vector("checkpoint_alias", persistent=True)
    raw = p.vector("checkpoint_raw", persistent=True)
    probe = p.vector("checkpoint_probe", persistent=True)
    actor = p.global_value("checkpoint_actor", 4, persistent=True)
    checked = p.global_value("checkpoint_checked", 2, persistent=True)
    offset = p.vector("fixture_offset", (1048576.125, -1048575.75, 512.5))
    zero = p.global_value("fixture_zero", 2)
    one = p.global_value("fixture_one", 2, 0x3f800000)
    scratch = p.vector("fixture_scratch")
    pointer = p.global_value("fixture_pointer", 7)
    field_name = p.text("fixture_field_name", "checkpoint_home")
    anchor_name = p.text("fixture_anchor_name", "checkpoint_anchor")
    alias_name = p.text("fixture_alias_name", "checkpoint_alias")
    failure = p.text("fixture_failure", "world position checkpoint fixture assertion failed")
    ready = p.text("fixture_ready", "World position fixture PASS\n")
    remapped = p.text("fixture_remapped", "World position fixture remap PASS\n")
    world = p.global_offsets["world"]
    p.function("worldspawn", len(p.statements))
    p.call("wp_register_field", (field_name, False))
    p.call("wp_register_global", (anchor_name, False))
    p.call("wp_register_global", (alias_name, False))
    p.call("wp_alloc", (world, False)); p.op("STORE_V", 1, anchor)
    p.call("wp_add", (anchor, True), (anchor, True), (offset, True))
    p.op("STORE_V", anchor, alias); p.op("STORE_V", anchor, raw)
    p.call("spawn"); p.op("STORE_ENT", 1, actor)
    p.op("ADDRESS", actor, home, pointer); p.op("STOREP_V", anchor, pointer)
    p.call("wp_setentity", (actor, False), (anchor, True))
    p.op("DONE")
    p.function("StartFrame", len(p.statements))
    p.call("wp_valid", (anchor, True)); p.assert_true(1, failure)
    p.op("EQ_V", anchor, alias, scratch); p.assert_true(scratch, failure)
    p.op("LOAD_V", actor, home, scratch)
    p.op("EQ_V", anchor, scratch, scratch); p.assert_true(scratch, failure)
    p.call("wp_valid", (probe, True))
    jump = p.op("IF", 1)
    p.call("wp_alloc", (world, False)); p.op("STORE_V", 1, probe)
    op, a, _, c = p.statements[jump]; p.statements[jump] = (op, a, len(p.statements) - jump, c)
    p.call("wp_getentity", (probe, True), (actor, False))
    p.call("wp_distance", (anchor, True), (probe, True))
    p.op("EQ_F", 1, zero, scratch); p.assert_true(scratch, failure)
    p.op("EQ_V", raw, anchor, scratch)
    jump = p.op("IF", scratch)
    p.call("wp_valid", (raw, True))
    p.op("NOT_F", 1, 0, scratch); p.assert_true(scratch, failure)
    p.call("dprint", (remapped, False)); p.op("STORE_V", anchor, raw)
    op, a, _, c = p.statements[jump]; p.statements[jump] = (op, a, len(p.statements) - jump, c)
    jump = p.op("IF", checked)
    p.call("dprint", (ready, False)); p.op("STORE_F", one, checked)
    op, a, _, c = p.statements[jump]; p.statements[jump] = (op, a, len(p.statements) - jump, c)
    p.op("DONE")
    return p.encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="new progs.dat path")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("xb") as stream:
        stream.write(generate(root))
    print(args.output.resolve())


if __name__ == "__main__":
    main()
