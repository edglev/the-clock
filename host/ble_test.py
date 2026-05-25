#!/usr/bin/env python3
import asyncio, sys, time
from bleak import BleakScanner, BleakClient

SERVICE_UUID    = "00000000-0000-a359-42f0-4467de900001"
STATE_CHAR_UUID = "00000000-0000-a359-42f0-4467de900002"
STATS_CHAR_UUID = "00000000-0000-a359-42f0-4467de900003"
ACTION_CHAR_UUID = "00000000-0000-a359-42f0-4467de900004"
STATES = {0: "IDLE", 1: "THINKING", 2: "WAITING", 3: "SUCCESS"}

async def find_char(client, uuid):
    for s in client.services:
        if s.uuid.lower() == SERVICE_UUID.lower() or SERVICE_UUID.lower() in s.uuid.lower():
            for c in s.characteristics:
                if c.uuid.lower() == uuid.lower():
                    return c
    return None

async def main():
    print("Scanning...")
    d = await BleakScanner.find_device_by_filter(lambda d, ad: d.name and "Agent-Viewer" in d.name, timeout=15)
    if not d: print("Not found"); return
    print(f"Found: {d.address}")

    async with BleakClient(d) as c:
        print(f"Connected: {c.is_connected}")
        await asyncio.sleep(0.5)

        state_char = await find_char(c, STATE_CHAR_UUID)
        stats_char = await find_char(c, STATS_CHAR_UUID)
        action_char = await find_char(c, ACTION_CHAR_UUID)

        if not state_char: print("State char not found!"); return
        print("State char OK")

        if stats_char: print("Stats char OK")
        if action_char:
            print("Action char OK")
            try:
                await c.start_notify(action_char, lambda s, d: print(f"[TOUCH] {d[0] if d else '?'}"))
            except:
                pass

        if "--cycle" in sys.argv:
            for s in range(4):
                print(f"State {s} ({STATES[s]})")
                await c.write_gatt_char(state_char, bytes([s]))
                await asyncio.sleep(3)
            await c.write_gatt_char(state_char, bytes([0]))
            print("Done")
        elif any(a.startswith("--state") for a in sys.argv):
            s = int(sys.argv[sys.argv.index("--state") + 1])
            await c.write_gatt_char(state_char, bytes([s]))
            print(f"State {s}")
        else:
            print("Commands: 0-3 = state, q = quit")
            while True:
                cmd = input("> ").strip()
                if cmd == "q": break
                try:
                    s = int(cmd)
                    if s in STATES:
                        await c.write_gatt_char(state_char, bytes([s]))
                        print(f"-> {STATES[s]}")
                except:
                    pass

asyncio.run(main())
