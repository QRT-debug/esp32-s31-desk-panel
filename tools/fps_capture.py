import serial, time, sys

ser = serial.Serial("COM5", 115200, timeout=1)
ser.reset_input_buffer()
t0 = time.time()
out = open(r"C:\Users\Admin\Desktop\sample_project\fps_capture.log", "wb")
while time.time() - t0 < 18:
    data = ser.read(4096)
    if data:
        out.write(data)
        out.flush()
out.close()
ser.close()
print("capture done")
