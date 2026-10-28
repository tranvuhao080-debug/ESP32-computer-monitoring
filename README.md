# ESP32 CyberDeck - Hệ Thống Giám Sát Phần Cứng & Live Desktop Streamer

Hệ thống giám sát hiệu năng phần cứng máy tính mini và truyền ảnh chụp màn hình trực tiếp từ **PC Linux** sang **ESP32** qua giao tiếp **Bluetooth Classic (RFCOMM)**, hiển thị trên màn hình TFT LCD 1.8" (ST7735).

---

## 📑 Mục Lục

* [Tính Năng Nổi Bật](#-tính-năng-nổi-bật)
* [Kiến Trúc Hệ Thống](#-kiến-trúc-hệ-thống)
* [Sơ Đồ Đấu Nối Phần Cứng](#-sơ-đồ-đấu-nối-phần-cứng)
* [Yêu Cầu Hệ Thống & Môi Trường (Bắt Buộc)](#-yêu-cầu-hệ-thống--môi-trường-bắt-buộc)
* [Hướng Dẫn Cài Đặt & Chạy Dự Án](#-hướng-dẫn-cài-đặt--chạy-dự-án)
  * [1. Nạp Firmware Cho ESP32](#1-nạp-firmware-cho-esp32)
  * [2. Cấu Hình & Chạy Server Python Trên Linux](#2-cấu-hình--chạy-server-python-trên-linux)
* [Giao Thức Truyền Dữ Liệu Nhị Phân](#-giao-thức-truyền-dữ-liệu-nhị-phân)
* [Xử Lý Lỗi Thường Gặp (Troubleshooting)](#-xử-lý-lỗi-thường-gặp-troubleshooting)

---

## 🚀 Tính Năng Nổi Bật

* **Giám Sát Tài Nguyên Trực Tiếp**: Hiển thị thời gian thực chỉ số CPU (dùng %, xung nhịp GHz, công suất tiêu thụ Watt), nhiệt độ CPU, dung lượng RAM (đã dùng / tổng cộng), mức tải iGPU Intel và lưu lượng mạng Upload/Download (KB/s, MB/s).
* **Live Active Window Streamer**: Chụp riêng khung hình cửa sổ ứng dụng đang active (được focus) trên Linux, tối ưu hóa màu về dạng RGB565 và stream mượt mà sang màn hình nhỏ của ESP32.
* **Chuyển Đổi 6 Màn Hình Giao Diện**: Dùng nút bấm vật lý để quay vòng qua 6 chế độ hiển thị:
  1. **MAIN / Overview**: Tổng quan CPU, RAM, iGPU.
  2. **CPU History**: Biểu đồ lịch sử tải CPU dạng btop.
  3. **RAM History**: Biểu đồ lịch sử dung lượng RAM.
  4. **iGPU History**: Biểu đồ lịch sử nhân đồ họa tích hợp Intel.
  5. **NET Traffic**: Biểu đồ băng thông Download & Upload.
  6. **Desktop / Active Window**: Luồng xem trực tiếp cửa sổ làm việc trên PC.
* **Tối Ưu Hiệu Năng & Khống Chế Deadlock**: Mã nguồn ESP32 sử dụng Hardware SPI (VSPI), xử lý dữ liệu qua State Machine không chặn (non-blocking) kết hợp Checksum để lọc bỏ khung hình hỏng khi nhiễu sóng Bluetooth.

---

## 🏗 Kiến Trúc Hệ Thống

```text
┌────────────────────────────────┐         Bluetooth RFCOMM         ┌────────────────────────────────┐
│            PC Linux            │ ───────────────────────────────> │          Mạch ESP32            │
│   (server.py / X11 Session)    │   Giao thức nhị phân (JSON/IMG) │  (esp32-cyber.ino Firmware)    │
└────────────────────────────────┘                                  └───────────────┬────────────────┘
                                                                                    │ SPI (Hardware)
                                                                                    ▼
                                                                    ┌────────────────────────────────┐
                                                                    │    Màn hình ST7735S (128x160)   │
                                                                    └────────────────────────────────┘
```

---

## 🔌 Sơ Đồ Đấu Nối Phần Cứng

### Danh Sách Linh Kiện
1. **Mạch ESP32 Dev Module** (30 pin hoặc 38 pin, hỗ trợ Bluetooth Classic).
2. **Màn Hình TFT LCD ST7735S** (Kích thước 1.8 inch, độ phân giải 128x160, giao tiếp SPI).
3. **Nút Bấm Vật Lý (Pushbutton)** 2 chân/4 chân.
4. Breadboard và dây cắm mạch (Jumper wires).

### Bảng Nối Chân (Pinout Configuration)

| Màn Hình ST7735 Pin | Chân ESP32 | Ghi Chú / Chức Năng |
| :--- | :--- | :--- |
| **VCC** | 3.3V hoặc 5V | Nguồn cấp cho màn hình |
| **GND** | GND | Chân mass chung |
| **CS** | GPIO 5 | SPI Chip Select |
| **RESET / RST** | GPIO 4 | Display Reset |
| **A0 / DC** | GPIO 21 | Data / Command Control |
| **SDA / MOSI** | GPIO 23 | Hardware SPI Master Out (VSPI_MOSI) |
| **SCK / SCL** | GPIO 18 | Hardware SPI Clock (VSPI_SCLK) |
| **LED / BL** | 3.3V | Nguồn đèn nền màn hình |

| Nút Bấm Pin | Chân ESP32 | Ghi Chú |
| :--- | :--- | :--- |
| **Chân 1** | GPIO 2 | Nút chuyển màn hình (Sử dụng internal `INPUT_PULLUP`) |
| **Chân 2** | GND | Chân mass |

---

## ⚙️ Yêu Cầu Hệ Thống & Môi Trường (Bắt Buộc)

Để hệ thống hoạt động trơn tru và không gặp lỗi khi chạy file Python Server trên PC, hệ thống máy chủ cần đáp ứng các điều kiện sau:

### 1. Máy Chủ Hiển Thị: Bắt Buộc Sử Dụng X11 / Xorg
* **Lý do**: File `server.py` sử dụng lệnh `xdotool getactivewindow` để lấy ID cửa sổ đang hoạt động và lệnh `import` của ImageMagick để chụp đúng vùng ảnh cửa sổ đó. Cả 2 công cụ này **chỉ hoạt động trên giao diện X11 (Xorg)**.
* ⚠️ **Lưu ý về Wayland**: Nếu bạn đang chạy các môi trường như GNOME hay KDE trên **Wayland**, tính năng chụp cửa sổ sẽ bị lỗi hoặc ra màn hình đen. Hãy chọn phiên bản **X11 / Xorg session** tại màn hình đăng nhập (hoặc sử dụng các Window Manager chạy X11 native như Openbox, i3wm, XFCE, Herbstluftwm...).

### 2. Cài Đặt Các Công Cụ Hệ Thống (System Utilities)
Cài đặt `xdotool` và `imagemagick` thông qua trình quản lý gói của hệ điều hành:

* **Arch Linux / Manjaro / EndeavourOS**:
  ```bash
  sudo pacman -S xdotool imagemagick
  ```
* **Ubuntu / Debian / Pop!_OS**:
  ```bash
  sudo apt-get update
  sudo apt-get install xdotool imagemagick
  ```

### 3. Quyền Truy Cập Cảm Biến & Bộ Nhớ Tạm
* **Công suất CPU (Intel RAPL)**: Script đọc chỉ số năng lượng tiêu thụ trực tiếp từ giao diện Kernel `/sys/class/powercap/intel-rapl`. Hãy đảm bảo máy tính sử dụng CPU Intel và Kernel hỗ trợ module `powercap`.
* **Shared Memory (`/dev/shm`)**: Script ghi file ảnh tạm thời vào ổ đĩa RAM `/dev/shm/cyberdeck_shot.png` nhằm tránh ghi đọc liên tục gây hại ổ cứng SSD/HDD và giúp đạt tốc độ truyền tải khung hình nhanh nhất.

---

## 🛠 Hướng Dẫn Cài Đặt & Chạy Dự Án

### 1. Nạp Firmware Cho ESP32

1. Mở phần mềm **Arduino IDE** và cài đặt gói bo mạch **ESP32** (phiên bản 2.x hoặc 3.x).
2. Vào **Tools** -> **Manage Libraries...** và cài đặt các thư viện sau:
   * `Adafruit GFX Library`
   * `Adafruit ST7735 and ST7789 Library`
   * `ArduinoJson` (Khuyến nghị dùng bản 6.x)
3. Mở file mã nguồn `esp32-cyber.ino`.
4. Tại mục **Tools**: Chọn đúng tên bo mạch (vd: `ESP32 Dev Module`) và Cổng COM tương ứng.
5. Tiến hành Biên dịch (Verify) và Nạp code (Upload) vào ESP32.

### 2. Cấu Hình & Chạy Server Python Trên Linux

1. Tải hoặc Clone repository này về PC:
   ```bash
   git clone https://github.com/your-username/esp32-cyberdeck.git
   cd esp32-cyberdeck
   ```

2. Tạo môi trường ảo Python (Virtual Environment) và cài đặt các thư viện phụ thuộc:
   ```bash
   python3 -m venv venv
   source venv/bin/activate
   pip install psutil numpy Pillow mss
   ```

3. Bật Bluetooth trên PC và ghép nối (Pair) với ESP32:
   * Cấp nguồn cho ESP32. Mạch sẽ tự động phát Bluetooth với tên: `ESP32_CyberDeck`.
   * Sử dụng công cụ Bluetooth trên PC hoặc qua dòng lệnh `bluetoothctl`:
     ```bash
     bluetoothctl
     power on
     scan on
     pair XX:XX:XX:XX:XX:XX   # Thay bằng địa chỉ MAC của ESP32
     trust XX:XX:XX:XX:XX:XX
     ```

4. Lấy địa chỉ MAC của ESP32 và cập nhật vào file `server.py`:
   Mở file `server.py`, tìm dòng khai báo `ESP32_MAC` và thay bằng địa chỉ Bluetooth MAC thực tế của ESP32:
   ```python
   ESP32_MAC = "6C:C8:40:92:24:B2"  # Thay bằng MAC của ESP32 của bạn
   ```

5. Chạy Server Python:
   ```bash
   python3 server.py
   ```

---

## 📡 Giao Thức Truyền Dữ Liệu Nhị Phân

Để tối ưu hóa băng thông Bluetooth RFCOMM hạn chế và phòng tránh rớt dữ liệu làm đơ màn hình, dữ liệu đóng gói theo định dạng nhị phân đóng khung (Binary Framing System):

```text
┌───────────────┬───────────────┬─────────────────────────┬───────────────────────────────┬───────────────┐
│  Magic Byte   │   Type Byte   │   Length (2 Bytes BE)   │         Payload Data          │   Checksum    │
│    (0xFF)     │ (0x01 / 0x02) │   Độ dài khung tin      │ (Chuỗi JSON hoặc Ảnh RGB565)  │ (Modulus 256) │
└───────────────┴───────────────┴─────────────────────────┴───────────────────────────────┴───────────────┘
```

* **Type `0x01` (TYPE_JSON)**: Chứa thông tin thông số hệ thống dưới dạng chuỗi JSON rút gọn.
* **Type `0x02` (TYPE_IMG)**: Chứa mảng dữ liệu pixel nguyên bản định dạng mảng RGB565 (độ phân giải 144x74, kích thước cố định 21,312 bytes).

---

## ❓ Xử Lý Lỗi Thường Gặp (Troubleshooting)

1. **Màn hình ESP32 treo ở thông báo `WAITING BT...`**:
   * Kiểm tra xem file `server.py` đã chạy trên PC chưa.
   * Kiểm tra xem địa chỉ `ESP32_MAC` trong file `server.py` đã chính xác chưa.
   * Xóa ghép nối Bluetooth cũ trên PC và tiến hành pair lại thiết bị `ESP32_CyberDeck`.

2. **Khung hình Stream bị đen hoặc hiển thị không đúng**:
   * Đảm bảo PC đang chạy trên phiên bản **X11 / Xorg**.
   * Kiểm tra xem ứng dụng `xdotool` và `imagemagick` đã được cài đặt chưa bằng cách gõ `xdotool -v` và `import -version` trên Terminal.

3. **Màn hình TFT đứng yên hoặc nháy liên tục khi chuyển đổi**:
   * Đảm bảo nguồn điện cấp cho ESP32 ổn định (khuyến nghị dùng cáp MicroUSB/Type-C chất lượng cao).
   * Kiểm tra lại các chân nối SPI (CS, DC, RST, MOSI, SCK) đã cắm chắc chắn chưa.
