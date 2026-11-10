#!/usr/bin/env python3
"""
Batch tool for SCS bus servos on a USB serial adapter: give every servo that
shares an ID a new ID, and zero them. Needs pyserial.

Servos sharing an ID all answer at once, so a garbled reply still counts as
"someone is there", and a single servo cannot be told apart from six.

Windows exe: pyinstaller -F -w tools/servo_tool.py
"""
import time
import tkinter as tk
from tkinter import messagebox, ttk

import serial
from serial.tools import list_ports

BAUD = 1000000
READ_TIMEOUT = 0.03
ADDR_ID = 0x05
ADDR_TORQUE = 40
ADDR_GOAL_POS = 0x2A
ADDR_LOCK = 0x37
ADDR_PRESENT = 0x38
CENTER = 2048
MOVE_SPEED = 1000  # steps/s, a goal speed of 0 would mean full speed


def frame_ok(rx, sid, data_len):
    n = data_len + 6
    return len(rx) >= n and rx[:4] == bytes([0xFF, 0xFF, sid, data_len + 2]) and ~sum(rx[2:n - 1]) & 0xFF == rx[n - 1]


class App:
    def __init__(self, root):
        self.root = root
        self.port = None
        self.busy = False
        self.one_running = False
        self.slide_pending = False
        root.title("DoitDuck 舵机工具")
        root.protocol("WM_DELETE_WINDOW", self.on_close)

        self.port_name = tk.StringVar()
        self.old_id = tk.StringVar(value="1")
        self.new_id = tk.StringVar(value="11")
        self.zero_id = tk.StringVar(value="11")
        self.count = tk.StringVar(value="舵机数量：未扫描")
        self.slide_text = tk.StringVar(value=str(CENTER))

        row = ttk.Frame(root, padding=8)
        row.grid(sticky="ew")
        ttk.Label(row, text="串口").pack(side="left")
        self.port_box = ttk.Combobox(row, textvariable=self.port_name, state="readonly", width=10)
        self.port_box.pack(side="left", padx=4)
        ttk.Button(row, text="刷新", command=self.refresh_ports).pack(side="left", padx=4)
        self.open_button = ttk.Button(row, text="打开", command=self.toggle_port)
        self.open_button.pack(side="left", padx=4)
        ttk.Button(row, text="扫描总线", command=lambda: self.run(self.scan)).pack(side="left", padx=4)

        ttk.Label(root, textvariable=self.count, font=("", 14, "bold")).grid(sticky="w", padx=8)

        row = ttk.LabelFrame(root, text="改 ID", padding=8)
        row.grid(sticky="ew", padx=8, pady=4)
        ttk.Label(row, text="原 ID").pack(side="left")
        ttk.Entry(row, textvariable=self.old_id, width=5).pack(side="left", padx=4)
        ttk.Label(row, text="新 ID").pack(side="left", padx=(8, 0))
        ttk.Entry(row, textvariable=self.new_id, width=5).pack(side="left", padx=4)
        ttk.Button(row, text="批量改 ID", command=lambda: self.run(self.set_id)).pack(side="left", padx=8)
        # One-by-one mode is disabled for now; drop state= to bring it back.
        self.one_button = ttk.Button(row, text="逐个接入", command=self.toggle_one, state="disabled")
        self.one_button.pack(side="left")

        row = ttk.LabelFrame(root, text="零位（2048）", padding=8)
        row.grid(sticky="ew", padx=8, pady=4)
        ttk.Label(row, text="ID").pack(side="left")
        ttk.Entry(row, textvariable=self.zero_id, width=5).pack(side="left", padx=4)
        ttk.Button(row, text="转到零位", command=lambda: self.run(self.goto_center)).pack(side="left", padx=4)
        ttk.Button(row, text="当前位置设为零位", command=lambda: self.run(self.set_center)).pack(side="left", padx=4)
        ttk.Button(row, text="卸力", command=lambda: self.run(self.release)).pack(side="left", padx=4)

        row = ttk.LabelFrame(root, text="位置控制（ID 同上，0～4095，拖动时上力）", padding=8)
        row.grid(sticky="ew", padx=8, pady=4)
        self.slider = ttk.Scale(row, from_=0, to=4095, value=CENTER, command=self.on_slide)
        self.slider.pack(side="left", fill="x", expand=True)
        ttk.Label(row, textvariable=self.slide_text, width=5).pack(side="left", padx=(8, 0))

        self.text = tk.Text(root, width=64, height=16, state="disabled")
        self.text.grid(sticky="nsew", padx=8, pady=8)
        root.columnconfigure(0, weight=1)
        root.rowconfigure(5, weight=1)

        self.refresh_ports()

    def on_close(self):
        self.one_running = False
        if self.busy:
            self.root.after(100, self.on_close)
        else:
            self.root.destroy()

    def log(self, message):
        self.text["state"] = "normal"
        self.text.insert("end", message + "\n")
        self.text.see("end")
        self.text["state"] = "disabled"

    def refresh_ports(self):
        ports = [p.device for p in list_ports.comports()]
        self.port_box["values"] = ports
        if self.port_name.get() not in ports:
            self.port_name.set(ports[0] if ports else "")

    def close_port(self):
        self.port.close()
        self.port = None
        self.open_button["text"] = "打开"

    def toggle_port(self):
        if self.busy:
            return
        if self.port:
            self.close_port()
            self.log("串口已关闭")
            return
        try:
            self.port = serial.Serial(self.port_name.get(), BAUD, timeout=READ_TIMEOUT)
        except serial.SerialException as e:
            self.log(f"打开失败：{e}")
            return
        self.open_button["text"] = "关闭"
        self.log(f"已打开 {self.port_name.get()}，{BAUD} bps")

    def run(self, action):
        if self.busy:
            return
        if not self.port:
            messagebox.showwarning("提示", "请先打开串口")
            return
        self.busy = True
        try:
            action()
        except ValueError:
            messagebox.showwarning("提示", "ID 必须是 0～253 的数字")
        except serial.SerialException as e:
            self.log(f"串口错误：{e}")
            self.close_port()
        self.busy = False

    def read_id(self, var):
        value = int(var.get())
        if not 0 <= value <= 253:
            raise ValueError
        return value

    def exchange(self, sid, fun, data=()):
        body = bytes([sid, len(data) + 2, fun, *data])
        tx = b"\xff\xff" + body + bytes([~sum(body) & 0xFF])
        self.port.reset_input_buffer()
        self.port.write(tx)
        rx = self.port.read(64)
        # Adapters that tie TX to RX hear their own request first.
        return rx[len(tx):] if rx.startswith(tx) else rx

    def ping(self, sid):
        return self.exchange(sid, 0x01)

    def write(self, sid, addr, *values):
        self.exchange(sid, 0x03, (addr, *values))

    def move(self, sid, pos):
        self.write(sid, ADDR_TORQUE, 1)
        self.write(sid, ADDR_GOAL_POS, pos & 0xFF, pos >> 8, 0, 0, MOVE_SPEED & 0xFF, MOVE_SPEED >> 8)

    def single_reply(self, sid, ping_rx):
        # Servos sharing an ID can agree on a ping; their positions rarely agree.
        pos = self.exchange(sid, 0x02, (ADDR_PRESENT, 2))
        return frame_ok(ping_rx, sid, 0) and frame_ok(pos, sid, 2)

    def change_id(self, old, new):
        self.write(old, ADDR_TORQUE, 0)
        self.write(old, ADDR_LOCK, 0)
        self.write(old, ADDR_ID, new)
        self.write(new, ADDR_LOCK, 1)
        if self.ping(old):
            return f"失败：ID {old} 还有回复，有舵机没改成功，请重试"
        if not self.ping(new):
            return f"失败：ID {new} 没有回复"
        return None

    def online_id(self):
        sid = self.read_id(self.zero_id)
        if self.ping(sid):
            return sid
        self.log(f"ID {sid} 没有回复")
        return None

    def scan(self):
        self.log("扫描 ID 0～253，约 8 秒...")
        found = shared = 0
        for sid in range(254):
            self.root.update()
            rx = self.ping(sid)
            if not rx:
                continue
            found += 1
            if self.single_reply(sid, rx):
                self.log(f"ID {sid}：在线")
            else:
                shared += 1
                self.log(f"ID {sid}：回复冲突，有多个舵机同 ID")
        if shared:
            self.count.set(f"舵机数量：至少 {found + shared} 个（{shared} 个 ID 有多个舵机，具体数量读不出）")
        else:
            self.count.set(f"舵机数量：{found} 个")
        self.log(f"扫描完成，{self.count.get()}")

    def set_id(self):
        old = self.read_id(self.old_id)
        new = self.read_id(self.new_id)
        if old == new:
            self.log("原 ID 和新 ID 相同")
            return
        if not self.ping(old):
            self.log(f"ID {old} 没有回复")
            return
        question = f"把总线上所有 ID {old} 的舵机改成 ID {new}？"
        if self.ping(new):
            question += f"\n\n注意：ID {new} 已经有舵机在线，改完会和它同 ID。"
        if not messagebox.askyesno("确认", question):
            return
        error = self.change_id(old, new)
        if error:
            self.log(error)
        else:
            self.log(f"完成：ID {old} 已全部改为 ID {new}（具体几个请看实际接了几个）")
            self.zero_id.set(str(new))

    def toggle_one(self):
        if self.one_running:
            self.one_running = False
        else:
            self.run(self.one_by_one)

    def one_by_one(self):
        old = self.read_id(self.old_id)
        new = self.read_id(self.new_id)
        if old == new:
            self.log("原 ID 和新 ID 相同")
            return
        if not messagebox.askyesno("确认", f"开始后，每接上一个 ID {old} 的舵机，都会自动改成 ID {new} 并计数。\n\n请一次只接一个，改好的可以留在总线上。"):
            return
        count = 0
        warned = False
        self.one_running = True
        self.one_button["text"] = "停止接入"
        self.count.set("舵机数量：已接入 0 个")
        self.zero_id.set(str(new))
        self.log(f"逐个接入：请接上一个 ID {old} 的舵机")
        try:
            while self.one_running:
                self.root.update()
                if not self.ping(old):
                    warned = False
                    continue
                # Let the connector settle before touching the servo's flash.
                time.sleep(0.5)
                if not self.single_reply(old, self.ping(old)):
                    if not warned:
                        self.log(f"ID {old} 回复冲突，可能同时接了多个，请一次只接一个")
                    warned = True
                    continue
                error = self.change_id(old, new)
                if error:
                    self.log(error)
                    break
                count += 1
                warned = False
                self.count.set(f"舵机数量：已接入 {count} 个")
                self.log(f"第 {count} 个已改为 ID {new}，请接下一个")
        finally:
            self.one_running = False
            self.one_button["text"] = "逐个接入"
        self.log(f"逐个接入结束，共 {count} 个")

    def on_slide(self, value):
        self.slide_text.set(str(int(float(value))))
        # A write takes one read timeout, so send the latest value at most every 100 ms.
        if not self.slide_pending:
            self.slide_pending = True
            self.root.after(100, self.send_slide)

    def send_slide(self):
        self.slide_pending = False
        if self.port:
            self.run(lambda: self.move(self.read_id(self.zero_id), int(self.slider.get())))

    def goto_center(self):
        sid = self.online_id()
        if sid is None:
            return
        self.move(sid, CENTER)
        self.slider.set(CENTER)
        self.log(f"ID {sid} 正在转到零位并保持上力，用完点「卸力」")

    def set_center(self):
        sid = self.online_id()
        if sid is None:
            return
        if not messagebox.askyesno("确认", f"把所有 ID {sid} 舵机的当前位置写成零位（2048）？这会改舵机内部的偏移。"):
            return
        self.write(sid, ADDR_TORQUE, 0)
        self.write(sid, ADDR_LOCK, 0)
        self.write(sid, ADDR_TORQUE, 128)
        time.sleep(0.1)
        self.write(sid, ADDR_LOCK, 1)
        rx = self.exchange(sid, 0x02, (ADDR_PRESENT, 2))
        if frame_ok(rx, sid, 2):
            self.log(f"完成：ID {sid} 读回位置 {rx[5] | rx[6] << 8}")
        else:
            self.log(f"已发送：ID {sid} 读不到位置（多个舵机同 ID 时回复冲突，属正常）")

    def release(self):
        sid = self.online_id()
        if sid is None:
            return
        self.write(sid, ADDR_TORQUE, 0)
        self.log(f"ID {sid} 已卸力")


root = tk.Tk()
App(root)
root.mainloop()
