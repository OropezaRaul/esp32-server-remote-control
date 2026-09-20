import os, fcntl, struct, time, select, sys

fd = os.open('/dev/ttyACM0', os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
TIOCMBIC = 0x5417
TIOCMBIS = 0x5416
TIOCM_DTR = 0x002
TIOCM_RTS = 0x004

fcntl.ioctl(fd, TIOCMBIC, struct.pack('I', TIOCM_DTR))
fcntl.ioctl(fd, TIOCMBIS, struct.pack('I', TIOCM_RTS))
time.sleep(0.1)
fcntl.ioctl(fd, TIOCMBIC, struct.pack('I', TIOCM_RTS))
fcntl.ioctl(fd, TIOCMBIS, struct.pack('I', TIOCM_DTR))

print("Reset pulse sent, waiting for boot...", flush=True)

t0 = time.time()
while time.time() - t0 < 22.0:
    r, _, _ = select.select([fd], [], [], 0.2)
    if r:
        try:
            data = os.read(fd, 1024)
            if data:
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
        except BlockingIOError:
            pass

os.close(fd)
