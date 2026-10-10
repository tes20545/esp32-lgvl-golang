package main

import (
	"fmt"
	"log"
	"strings"
	"syscall"
	"time"
	"unsafe"

	"github.com/shirou/gopsutil/v3/cpu"
	"github.com/shirou/gopsutil/v3/disk"
	"github.com/shirou/gopsutil/v3/mem"
	"go.bug.st/serial"
)

// Windows API สำหรับเช็คหน้าต่างที่กำลังใช้งานอยู่ (Active Window)
var (
	user32                  = syscall.NewLazyDLL("user32.dll")
	procGetForegroundWindow = user32.NewProc("GetForegroundWindow")
	procGetWindowTextW      = user32.NewProc("GetWindowTextW")

	enumWindows     = user32.NewProc("EnumWindows")
	isWindowVisible = user32.NewProc("IsWindowVisible")
)

func HWND() uintptr {
	hwnd, _, _ := procGetForegroundWindow.Call()
	if hwnd == 0 {
		return 0
	}
	return hwnd
}

func getActiveWindowTitle() string {
	var buf [256]uint16
	procGetWindowTextW.Call(HWND(), uintptr(unsafe.Pointer(&buf[0])), uintptr(len(buf)))
	
	title := syscall.UTF16ToString(buf[:])
	if title == "" {
		return "Desktop"
	}
	return title
}

// รายชื่อคำค้นหาเกมยอดฮิตเพื่อเอาไว้เช็คว่ากำลังเล่นเกมไหนอยู่
var knownGames = []string{
	"Cyberpunk", "Dota 2", "Valorant", "CS2", "Counter-Strike",
	"Genshin Impact", "League of Legends", "GTA V", "Minecraft", "Elden Ring", "Wuthering Waves",
}

func detectGame() (string, bool) {
	gameDetected := "None"

	cb := syscall.NewCallback(func(hwnd uintptr, lparam uintptr) uintptr {
		// 1. เช็คก่อนว่าหน้าต่างนี้มองเห็นอยู่ไหม (ซ่อนอยู่หลังบ้านหรือเป็นระบบรันเบื้องหลังไหม)
		isVisible, _, _ := isWindowVisible.Call(hwnd)
		if isVisible == 0 {
			return 1 // 1 หมายถึงให้วนลูปหาตัวถัดไปเรื่อยๆ
		}

		// 2. ดึงชื่อหัวข้อของหน้าต่างนั้นๆ
		var buf [256]uint16
		procGetWindowTextW.Call(hwnd, uintptr(unsafe.Pointer(&buf[0])), uintptr(len(buf)))
		title := syscall.UTF16ToString(buf[:])

		// 3. ถ้ามีชื่อให้พิมพ์ออกมา
		for _, game := range knownGames {
			if strings.Contains(strings.ToLower(title), strings.ToLower(game)) {
				gameDetected = game
				return 0 // 0 หมายถึงเจอแล้วให้หยุดลูป
			}
		}

		return 1 // สั่งให้ลูปทำงานต่อจนครบทุกหน้าต่าง
	})

	enumWindows.Call(cb, 0)
	
	return gameDetected, gameDetected != "None"
}

func main() {
	portName := "COM13" // ⚠️ เปลี่ยนเป็นพอร์ต COM ของคุณ
	mode := &serial.Mode{BaudRate: 115200}

	port, err := serial.Open(portName, mode)
	if err != nil {
		log.Fatalf("ไม่สามารถเปิดพอร์ต %s ได้: %v", portName, err)
	}
	defer port.Close()

	fmt.Printf("🚀 เริ่มต้นระบบส่งข้อมูลไปยัง %s สำเร็จ!\n", portName)

	var currentGame = "None"
	var gameStartTime time.Time
	var gameDuration string = "00:00"

	for {
		// 1. ดึงข้อมูล CPU และ RAM
		cpuPercent, _ := cpu.Percent(1*time.Second, false)
		var cpuVal float64
		if len(cpuPercent) > 0 {
			cpuVal = cpuPercent[0]
		}

		vMem, _ := mem.VirtualMemory()
		var ramVal float64
		if vMem != nil {
			ramVal = vMem.UsedPercent
		}

		// 3. Disk usage (ไดรฟ์ C:) — ใช้แทนอุณหภูมิ RAM ที่ไม่มีเซนเซอร์
		var diskVal float64
		if du, err := disk.Usage("C:"); err == nil && du != nil {
			diskVal = du.UsedPercent
		}

		// TODO: ต่อแหล่งข้อมูลจริงภายหลัง (NVIDIA NVML / LibreHardwareMonitor WMI)
		//       ตอนนี้ส่งค่า fallback "--" เพื่อให้ layout ฝั่ง ESP32 ทำงานได้ครบ
		gpuUsage := "--"
		cpuTemp := "--"
		gpuTemp := "--"

		// 4. ตรวจสอบ Active Window เพื่อหาเกมและจับเวลา Timer
		windowTitle := getActiveWindowTitle()
		detectedGame, isPlayingGame := detectGame()

		if isPlayingGame {
			if currentGame != detectedGame {
				// เปลี่ยนเกมหรือเพิ่งเริ่มเข้าเกม
				currentGame = detectedGame
				gameStartTime = time.Now()
			}
			// คำนวณเวลาที่เล่นไป
			duration := time.Since(gameStartTime)
			hours := int(duration.Hours())
			minutes := int(duration.Minutes()) % 60
			seconds := int(duration.Seconds()) % 60
			if hours > 0 {
				gameDuration = fmt.Sprintf("%02d:%02d:%02d", hours, minutes, seconds)
			} else {
				gameDuration = fmt.Sprintf("%02d:%02d", minutes, seconds)
			}
		} else {
			currentGame = "None"
			gameDuration = "00:00"
		}

		// สมมติดึงชื่อเพลงจากหน้าต่างปัจจุบัน (ถ้าเปิด YouTube / Spotify มักจะมีชื่อเพลงโชว์ที่ Title)
		// ESP32 media slide wraps the title over two lines, so allow more characters here.
		songName := windowTitle
		if len(songName) > 48 {
			songName = songName[:45] + "..."
		}

		// 5. แพ็กข้อมูลส่งไปให้ ESP32
		// รูปแบบ: CPU:xx.x,CPUT:xx,GPU:xx.x,GPUT:xx,RAM:xx.x,DISK:xx.x,Game:Name,Time:MM:SS,Song:Title\n
		data := fmt.Sprintf("CPU:%.1f,CPUT:%s,GPU:%s,GPUT:%s,RAM:%.1f,DISK:%.1f,Game:%s,Time:%s,Song:%s\n",
			cpuVal, cpuTemp, gpuUsage, gpuTemp, ramVal, diskVal, currentGame, gameDuration, songName)

		_, err = port.Write([]byte(data))
		if err != nil {
			log.Printf("Error writing to serial: %v", err)
		}

		fmt.Printf("📤 ส่งข้อมูล: %s", data)
	}
}
