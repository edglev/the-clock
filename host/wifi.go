package main

import (
	"encoding/base64"
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"strings"
	"time"

	"tinygo.org/x/bluetooth"
)

const wifiConfigChunkLen = 180

type deviceCloudConfig struct {
	SSID        string `json:"ssid,omitempty"`
	WifiPass    string `json:"wifi_password,omitempty"`
	Region      string `json:"region,omitempty"`
	MQTTBroker  string `json:"mqtt_broker,omitempty"`
	AccessToken string `json:"access_token,omitempty"`
	UserID      string `json:"user_id,omitempty"`
	DeviceID    string `json:"device_id,omitempty"`
	DeviceName  string `json:"device_name,omitempty"`
	DeviceModel string `json:"device_model,omitempty"`
	DeviceProd  string `json:"device_product,omitempty"`
	GitLabURL   string `json:"gitlab_url,omitempty"`
	GitLabToken string `json:"gitlab_pat,omitempty"`
}

func runWifiSetup(args []string) error {
	fs := flag.NewFlagSet("wifi-setup", flag.ContinueOnError)
	ssid := fs.String("ssid", "", "Wi-Fi SSID for the ESP32")
	password := fs.String("password", "", "Wi-Fi password for the ESP32")
	bambuConfig := fs.String("bambu-config", bambuConfigPath(), "Path to saved Bambu config")
	if err := fs.Parse(args); err != nil {
		return err
	}

	if strings.TrimSpace(*ssid) == "" {
		line, err := promptLine("Wi-Fi SSID: ")
		if err != nil {
			return err
		}
		*ssid = strings.TrimSpace(line)
	}
	if strings.TrimSpace(*password) == "" {
		secret, err := promptSecret("Wi-Fi password: ")
		if err != nil {
			return err
		}
		*password = secret
	}

	cfg, err := readBambuConfig(expandHome(*bambuConfig))
	if err != nil {
		return fmt.Errorf("read Bambu config: %w", err)
	}
	applyBambuEnv(&cfg)
	if cfg.Region == "" {
		cfg.Region = "global"
	}
	if cfg.MQTTBroker == "" {
		cfg.MQTTBroker = bambuMQTTBroker(cfg.Region)
	}
	if cfg.AccessToken == "" || cfg.UserID == "" || cfg.DeviceID == "" || cfg.MQTTBroker == "" {
		return fmt.Errorf("Bambu config missing; run `%s bambu-login` first", os.Args[0])
	}

	deviceCfg := deviceCloudConfig{
		SSID:        strings.TrimSpace(*ssid),
		WifiPass:    *password,
		Region:      normalizeBambuRegion(cfg.Region),
		MQTTBroker:  cfg.MQTTBroker,
		AccessToken: cfg.AccessToken,
		UserID:      cfg.UserID,
		DeviceID:    cfg.DeviceID,
		DeviceName:  cfg.DeviceName,
		DeviceModel: cfg.DeviceModel,
		DeviceProd:  cfg.DeviceProduct,
	}
	if err := sendDeviceCloudConfig(deviceCfg); err != nil {
		return err
	}
	fmt.Printf("Sent Wi-Fi and Bambu Cloud config for %s over encrypted BLE\n", firstNonEmpty(cfg.DeviceName, cfg.DeviceID))
	return nil
}

func sendDeviceCloudConfig(deviceCfg deviceCloudConfig) error {
	data, err := json.Marshal(deviceCfg)
	if err != nil {
		return err
	}
	if len(data) > 4096 {
		return fmt.Errorf("device config is too large: %d bytes", len(data))
	}

	if err := adapter.Enable(); err != nil {
		return fmt.Errorf("enable bluetooth adapter: %w", err)
	}
	device, err := connectProvisioningDevice()
	if err != nil {
		return err
	}
	defer device.Disconnect()

	srvUUID, _ := bluetooth.ParseUUID(svcUUID)
	maUUID, _ := bluetooth.ParseUUID(multiUUID)
	services, err := device.DiscoverServices([]bluetooth.UUID{srvUUID})
	if err != nil || len(services) == 0 {
		return fmt.Errorf("Agent-Viewer BLE service not found: %w", err)
	}
	chars, err := services[0].DiscoverCharacteristics([]bluetooth.UUID{maUUID})
	if err != nil || len(chars) == 0 {
		return fmt.Errorf("Agent-Viewer multi characteristic not found: %w", err)
	}

	encoded := base64.StdEncoding.EncodeToString(data)
	count := (len(encoded) + wifiConfigChunkLen - 1) / wifiConfigChunkLen
	for seq := 0; seq < count; seq++ {
		start := seq * wifiConfigChunkLen
		end := start + wifiConfigChunkLen
		if end > len(encoded) {
			end = len(encoded)
		}
		payload := fmt.Sprintf("C\t%d\t%d\t%s", seq, count, encoded[start:end])
		if err := writeCharacteristic(chars[0], []byte(payload)); err != nil {
			return fmt.Errorf("send config chunk %d/%d: %w", seq+1, count, err)
		}
		time.Sleep(80 * time.Millisecond)
	}

	return nil
}

func connectProvisioningDevice() (bluetooth.Device, error) {
	if configured, ok := configuredAddress(); ok {
		fmt.Printf("[ble] connecting to configured Agent-Viewer address %s\n", configured.String())
		return adapter.Connect(configured, bluetooth.ConnectionParams{})
	}

	fmt.Println("[ble] scanning for Agent-Viewer...")
	found := make(chan bluetooth.ScanResult, 1)
	if err := adapter.Scan(func(ad *bluetooth.Adapter, r bluetooth.ScanResult) {
		if r.LocalName() == "Agent-Viewer" {
			ad.StopScan()
			select {
			case found <- r:
			default:
			}
		}
	}); err != nil {
		adapter.StopScan()
		return bluetooth.Device{}, err
	}

	var result bluetooth.ScanResult
	select {
	case result = <-found:
	case <-time.After(15 * time.Second):
		adapter.StopScan()
		return bluetooth.Device{}, fmt.Errorf("scan timeout")
	}

	fmt.Printf("[ble] connecting to %s\n", result.Address.String())
	return adapter.Connect(result.Address, bluetooth.ConnectionParams{})
}
