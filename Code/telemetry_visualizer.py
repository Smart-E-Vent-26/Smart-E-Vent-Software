#!/usr/bin/env python3
"""
telemetry_visualizer.py — Real-Time Breath Telemetry Visualizer
Smart E-Ventilator Project

Usage:
  1. Close Arduino Serial Monitor (it locks the port!)
  2. Run: python telemetry_visualizer.py
  3. Close the graph window to stop.

Features:
  - 3 real-time subplots: Flow, Pressure, Volume
  - Tracks both Physical Flow and Virtual (Calculated) Flow
  - CSV logging with ADC and voltage columns
  - Background serial reader for smooth graphing
  - Terminal input forwarded to Arduino (type commands + Enter)
"""

import serial
import serial.tools.list_ports
import sys
import os
import re
import csv
import time
import threading
from collections import deque

import matplotlib
matplotlib.use('TkAgg')
import matplotlib.pyplot as plt

# ---- Configuration ----
BAUD = 115200
OUTPUT_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'telemetry_data.csv')

# ---- Shared Data Queues (thread-safe via GIL for simple appends) ----
MAX_POINTS = 5000
timestamps     = deque(maxlen=MAX_POINTS)
flow_data      = deque(maxlen=MAX_POINTS)
calc_flow_data = deque(maxlen=MAX_POINTS)  # ADDED: Virtual Flow Queue
vol_data       = deque(maxlen=MAX_POINTS)
pressure_data  = deque(maxlen=MAX_POINTS)

# ---- Regex Parsers ----
regex_flow      = re.compile(r'Flow=([-\d.]+)')
regex_calc_flow = re.compile(r'CalcFlow=([-\d.]+)') # ADDED: Regex for CalcFlow
regex_vol       = re.compile(r'Vol=([-\d.]+)')
regex_pressure  = re.compile(r'Pressure=([-\d.]+)')
regex_flow_adc  = re.compile(r'FlowADC=(\d+)')
regex_flow_volt = re.compile(r'FlowVolt=([-\d.]+)')


def find_arduino_port():
    """Auto-detect Arduino COM port."""
    ports = serial.tools.list_ports.comports()
    for p in ports:
        desc = (p.description or '').lower()
        if 'arduino' in desc or 'ch340' in desc or 'usb-serial' in desc or 'usbserial' in desc:
            return p.device
    if ports:
        return ports[0].device
    return None


def serial_reader(ser, stop_event, csv_writer, csv_file, start_time):
    """Background thread to read serial, parse regex, and write to CSV without lagging the graph."""
    while not stop_event.is_set():
        try:
            if ser.in_waiting:
                raw = ser.readline()
                line = raw.decode('utf-8', errors='ignore').strip()

                if line:
                    # Echo to console
                    print(line)

                    # 1. Parse Data
                    f_match  = regex_flow.search(line)
                    cf_match = regex_calc_flow.search(line)
                    v_match  = regex_vol.search(line)
                    p_match  = regex_pressure.search(line)

                    # Check which phase the ventilator is in
                    is_exhaling = "EXH" in line or "PAU" in line

                    if f_match and v_match:
                        try:
                            f_val = float(f_match.group(1))
                            cf_val = float(cf_match.group(1)) if cf_match else 0.0
                            v_val = float(v_match.group(1))
                            p_val = float(p_match.group(1)) if p_match else 0.0
                            t_val = (time.time() - start_time) * 1000.0  



                            # 2. Append to graph queues
                            timestamps.append(t_val)
                            flow_data.append(f_val)
                            calc_flow_data.append(cf_val) # ADDED: Queue CalcFlow
                            vol_data.append(v_val)
                            pressure_data.append(p_val)

                            # 3. Save to CSV immediately (Added CalcFlow to logging)
                            csv_writer.writerow([
                                f"{t_val:.1f}",
                                f"{f_val:.2f}",
                                f"{cf_val:.2f}",
                                f"{v_val:.1f}",
                                f"{p_val:.3f}",
                                fa_val,
                                fv_val
                            ])
                            csv_file.flush()
                        except ValueError:
                            pass

        except serial.SerialException:
            print("\n[!] Serial disconnected.")
            break
        except Exception:
            pass

        time.sleep(0.001)


def keyboard_writer(ser, stop_event):
    """Background thread to allow terminal input while graph is rendering."""
    while not stop_event.is_set():
        try:
            cmd = sys.stdin.readline()
            if cmd:
                ser.write(cmd.encode('utf-8'))
        except Exception:
            break


def open_csv_with_retry(base_path, max_attempts=10):
    """Open CSV file for writing. If locked, try numbered alternatives."""
    base = base_path.replace('.csv', '')
    for suffix in range(max_attempts + 1):
        if suffix == 0:
            file_path = base_path
        else:
            file_path = f"{base}_{suffix}.csv"
        try:
            csv_file = open(file_path, 'w', newline='', encoding='utf-8')
            if suffix > 0:
                print(f"[!] {os.path.basename(base_path)} is locked, saving to {os.path.basename(file_path)} instead.")
            return csv_file, file_path
        except PermissionError:
            continue

    print(f"ERROR: Could not create CSV file after {max_attempts} attempts.")
    return None, None


def main():
    print("=" * 50)
    print("  TELEMETRY VISUALIZER (DUAL FLOW)")
    print("  Smart E-Ventilator")
    print("=" * 50)
    print()

    port = find_arduino_port()
    if not port:
        print("ERROR: No COM ports found!")
        sys.exit(1)

    print(f"Connecting to {port} at {BAUD} baud...")
    try:
        ser = serial.Serial(port, BAUD, timeout=1)
    except serial.SerialException:
        print(f"ERROR: Cannot open {port}")
        sys.exit(1)

    time.sleep(2)
    ser.reset_input_buffer()

    csv_file, actual_file_path = open_csv_with_retry(OUTPUT_FILE)
    if csv_file is None:
        ser.close()
        sys.exit(1)

    csv_writer = csv.writer(csv_file)
    csv_writer.writerow(['time_ms', 'flow_physical_Lmin', 'flow_virtual_Lmin', 'vol_mL', 'pressure_kPa', 'flow_adc', 'flow_voltage_V'])
    print(f"Logging to: {actual_file_path}")

    start_time = time.time()
    stop_event = threading.Event()

    reader_thread = threading.Thread(target=serial_reader, args=(ser, stop_event, csv_writer, csv_file, start_time), daemon=True)
    reader_thread.start()

    kb_thread = threading.Thread(target=keyboard_writer, args=(ser, stop_event), daemon=True)
    kb_thread.start()

    # ---- Setup Matplotlib Graph ----
    plt.style.use('dark_background')
    plt.ion()

    fig, (ax1, ax2, ax3) = plt.subplots(3, 1, figsize=(10, 10))
    fig.canvas.manager.set_window_title('Telemetry Visualizer')

    # Flow Subplot (NOW WITH TWO LINES)
    line_flow, = ax1.plot([], [], color='cyan', linewidth=2, label='Physical Flow')
    line_calc_flow, = ax1.plot([], [], color='orange', linewidth=2, linestyle=':', label='Virtual Flow')
    ax1.set_title('Real-Time Flow (L/min)', color='white')
    ax1.set_ylabel('Flow (L/min)')
    ax1.grid(True, color='#333333')
    ax1.axhline(0, color='gray', linestyle='--')
    ax1.legend(loc='upper right', facecolor='black', edgecolor='white')

    # Pressure Subplot
    line_pressure, = ax2.plot([], [], color='magenta', linewidth=2)
    ax2.set_title('Airway Pressure (kPa)', color='white')
    ax2.set_ylabel('Pressure (kPa)')
    ax2.grid(True, color='#333333')
    ax2.axhline(0, color='gray', linestyle='--')

    # Volume Subplot
    line_vol, = ax3.plot([], [], color='lime', linewidth=2)
    ax3.set_title('Delivered Tidal Volume (mL)', color='white')
    ax3.set_ylabel('Volume (mL)')
    ax3.set_xlabel('Time (ms)')
    ax3.grid(True, color='#333333')

    plt.tight_layout()
    plt.show(block=False)

    try:
        while plt.fignum_exists(fig.number):
            if len(timestamps) > 1:
                ts_list = list(timestamps)
                fl_list = list(flow_data)
                cfl_list = list(calc_flow_data) # ADDED: Virtual flow list
                pr_list = list(pressure_data)
                vl_list = list(vol_data)

                # Update Flow Lines
                line_flow.set_data(ts_list, fl_list)
                line_calc_flow.set_data(ts_list, cfl_list) # ADDED: Draw virtual flow
                ax1.set_xlim(ts_list[0], ts_list[-1])
                
                # Dynamic scaling to fit both flow lines
                min_f = min(min(fl_list), min(cfl_list))
                max_f = max(max(fl_list), max(cfl_list))
                ax1.set_ylim(min_f - 5, max_f + 5)

                # Update Pressure Line
                line_pressure.set_data(ts_list, pr_list)
                ax2.set_xlim(ts_list[0], ts_list[-1])
                min_p, max_p = min(pr_list), max(pr_list)
                margin_p = max(0.5, (max_p - min_p) * 0.2)
                ax2.set_ylim(min_p - margin_p, max_p + margin_p)

                # Update Volume Line
                line_vol.set_data(ts_list, vl_list)
                ax3.set_xlim(ts_list[0], ts_list[-1])
                min_v, max_v = min(vl_list), max(vl_list)
                ax3.set_ylim(min_v - 10, max_v + 50 if max_v > 0 else 100)

            plt.pause(0.04)
    except KeyboardInterrupt:
        pass

    print("\nClosing Visualizer...")
    stop_event.set()
    ser.close()
    csv_file.close()
    print(f"Data saved to {actual_file_path}")

if __name__ == '__main__':
    main()