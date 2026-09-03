KOYODA Face Step 2 - Blink
==========================
เป้าหมายขั้นนี้: ให้ KOYODA แสดงหน้า Idle และกะพริบตาอัตโนมัติ

ไฟล์ภาพ 466x466
- assets/koyoda_idle_466.png
- assets/koyoda_half_466.png
- assets/koyoda_closed_466.png

ลำดับแอนิเมชัน
Idle 3 วินาที
→ Half Blink 60 ms
→ Closed 90 ms
→ Half Blink 60 ms
→ Idle
แล้ววนซ้ำ

ขั้นนี้ยังไม่มี
- Touch
- Emotion อื่น
- Wi-Fi
- Mic
- Speaker
- AI

สภาพแวดล้อม
- ESP-IDF 5.5.4
- Waveshare BSP 3.0.1
- LVGL 9.4.x

วิธีลอง
1) แตก ZIP เช่น C:\ESP\KOYODA-Face-Step2-Blink
2) เปิด ESP-IDF 5.5.4 Command Prompt
3) cd C:\ESP\KOYODA-Face-Step2-Blink
4) idf.py set-target esp32s3
5) idf.py fullclean
6) idf.py build

ถ้า build ผ่าน:
7) idf.py -p COMx flash monitor

ผลที่ควรเห็น:
KOYODA อยู่หน้า Idle แล้วกะพริบตาทุกประมาณ 3 วินาที
