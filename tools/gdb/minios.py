# SPDX-License-Identifier: MIT
"""gdb helpers for minios (loaded by tools/gdb/gdbinit).

  minios-tasks         every task: pid, state, name, runtime, run-queue flag
  minios-ptwalk VA     walk the current page tables for VA, decoding each level

Both read kernel memory through the running kernel's own mappings: page
tables are reached via the direct map (physical + HHDM_BASE), so use them
once the kernel has switched to its own tables (any time after mm_init).
"""

import gdb

HHDM_BASE = 0xFFFF800000000000
PTE_ADDR = 0x000FFFFFFFFFF000
STATES = {0: "RUNNING", 1: "RUNNABLE", 2: "BLOCKED", 3: "ZOMBIE"}


def read_u64(addr: int) -> int:
    data = gdb.selected_inferior().read_memory(addr, 8)
    return int.from_bytes(bytes(data), "little")


class Tasks(gdb.Command):
    """List all tasks: minios-tasks"""

    def __init__(self) -> None:
        super().__init__("minios-tasks", gdb.COMMAND_USER)

    def invoke(self, arg: str, from_tty: bool) -> None:
        task_type = gdb.lookup_type("struct task")
        offset = int(gdb.parse_and_eval("&((struct task *)0)->all_link"))
        head = gdb.parse_and_eval("&all_tasks")
        current = int(gdb.parse_and_eval("current_task"))
        node = head["next"]
        print(f"{'':2}{'PID':>4}  {'STATE':<9} {'NAME':<16} {'TICKS':>7} {'SWITCHES':>9}  RQ")
        while int(node) != int(head):
            task = gdb.Value(int(node) - offset).cast(task_type.pointer()).dereference()
            mark = "*" if int(node) - offset == current else " "
            state = STATES.get(int(task["state"]), "?")
            name = task["name"].string()
            print(f"{mark:2}{int(task['pid']):>4}  {state:<9} {name:<16} "
                  f"{int(task['runtime_ticks']):>7} {int(task['switches_in']):>9}  "
                  f"{'yes' if bool(task['on_rq']) else 'no'}")
            node = node["next"]


def decode(entry: int) -> str:
    bits = []
    bits.append("W" if entry & 2 else "R")
    bits.append("U" if entry & 4 else "S")
    bits.append("NX" if entry >> 63 else "X")
    if entry & (1 << 7):
        bits.append("HUGE")
    if entry & (1 << 8):
        bits.append("G")
    return " ".join(bits)


class PtWalk(gdb.Command):
    """Walk the page tables for an address: minios-ptwalk ADDRESS"""

    def __init__(self) -> None:
        super().__init__("minios-ptwalk", gdb.COMMAND_USER)

    def invoke(self, arg: str, from_tty: bool) -> None:
        va = int(gdb.parse_and_eval(arg)) & 0xFFFFFFFFFFFFFFFF
        table = int(gdb.parse_and_eval("$cr3")) & PTE_ADDR
        print(f"va {va:#018x}, cr3 {table:#x}")
        for level, name, shift in ((4, "PML4", 39), (3, "PDPT", 30), (2, "PD", 21), (1, "PT", 12)):
            index = (va >> shift) & 0x1FF
            entry = read_u64(HHDM_BASE + table + index * 8)
            if not entry & 1:
                print(f"  {name:<4}[{index:3}] = {entry:#018x}  not present")
                return
            print(f"  {name:<4}[{index:3}] = {entry:#018x}  {decode(entry)}")
            if level == 1 or entry & (1 << 7):
                page = 1 << shift
                phys = (entry & PTE_ADDR & ~(page - 1)) | (va & (page - 1))
                print(f"  -> phys {phys:#x} ({decode(entry)}, {page >> 10} KiB page)")
                return
            table = entry & PTE_ADDR


Tasks()
PtWalk()
