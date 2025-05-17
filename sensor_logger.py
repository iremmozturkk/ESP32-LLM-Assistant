import socket

UDP_IP = "0.0.0.0"
UDP_PORT = 4210
LOG_FILE = "sensor_log.txt"

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind((UDP_IP, UDP_PORT))

print(f"📡 Sensör UDP dinleyici başlatıldı ({UDP_PORT})")

while True:
    data, addr = sock.recvfrom(1024)
    decoded = data.decode("utf-8").strip()
    print(f"{addr} > {decoded}")
    with open(LOG_FILE, "a") as f:
        f.write(decoded + "\n")
