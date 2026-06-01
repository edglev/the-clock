package main

import (
	"bufio"
	"bytes"
	"crypto/tls"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"math"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
	"golang.org/x/term"
)

const (
	maxPrinterStateLen    = 16
	maxPrinterJobLen      = 48
	maxPrinterMaterialLen = 24
	maxPrinterTempLen     = 8
	maxPrinterSourceLen   = 16
	amsTrayCount          = 4
	maxAMSMaterialLen     = 12
	maxAMSColorLen        = 8
	maxAMSHumidityLen     = 8
	maxAMSRemainingLen    = 4
	printerSendInterval   = 30 * time.Second
	printerStaleAfter     = 2 * time.Minute
	printerReconnectMin   = 30 * time.Second
	printerReconnectMax   = 5 * time.Minute
	printerStableRunReset = 5 * time.Minute
)

type printerStatus struct {
	State           string
	ProgressPercent int
	EtaSeconds      int
	Layer           int
	Layers          int
	NozzleC         string
	BedC            string
	ChamberC        string
	Job             string
	Material        string
	Source          string
	Updated         time.Time
}

type amsTrayStatus struct {
	Material         string
	Color            string
	RemainingPercent int
}

type amsStatus struct {
	ActiveSlot int
	Humidity   string
	Trays      [amsTrayCount]amsTrayStatus
	Updated    time.Time
}

type bambuConfig struct {
	Region           string `json:"region"`
	APIBase          string `json:"api_base"`
	MQTTBroker       string `json:"mqtt_broker"`
	Email            string `json:"email,omitempty"`
	AccessToken      string `json:"access_token"`
	RefreshToken     string `json:"refresh_token,omitempty"`
	TokenExpiresAt   int64  `json:"token_expires_at,omitempty"`
	UserID           string `json:"user_id"`
	DeviceID         string `json:"device_id"`
	DeviceName       string `json:"device_name,omitempty"`
	DeviceModel      string `json:"device_model,omitempty"`
	DeviceProduct    string `json:"device_product,omitempty"`
	LastLoginUnixSec int64  `json:"last_login_unix_sec,omitempty"`
}

type bambuDevice struct {
	ID          string
	Name        string
	Model       string
	ProductName string
	Online      bool
}

var (
	printerMu      sync.Mutex
	currentPrinter = printerStatus{
		State:           "offline",
		ProgressPercent: -1,
		EtaSeconds:      -1,
		Layer:           -1,
		Layers:          -1,
		Job:             "Run bambu-login",
		Source:          "Cloud",
	}
	currentAMS      = amsStatus{ActiveSlot: -1}
	currentAMSValid = false
)

func runBambuLogin(args []string) error {
	fs := flag.NewFlagSet("bambu-login", flag.ContinueOnError)
	region := fs.String("region", envDefault("AGENT_VIEWER_BAMBU_REGION", "global"), "Bambu region: global or china")
	email := fs.String("email", "", "Bambu account email")
	password := fs.String("password", "", "Bambu account password")
	token := fs.String("token", "", "Existing Bambu Cloud access token")
	deviceID := fs.String("device", "", "Printer device serial to use")
	configPath := fs.String("config", bambuConfigPath(), "Path to save Bambu config")
	if err := fs.Parse(args); err != nil {
		return err
	}

	cfg := bambuConfig{
		Region:     normalizeBambuRegion(*region),
		APIBase:    bambuAPIBase(*region),
		MQTTBroker: bambuMQTTBroker(*region),
		Email:      strings.TrimSpace(*email),
		DeviceID:   strings.TrimSpace(*deviceID),
	}

	if strings.TrimSpace(*token) != "" {
		cfg.AccessToken = strings.TrimSpace(*token)
	} else {
		if cfg.Email == "" {
			line, err := promptLine("Bambu email: ")
			if err != nil {
				return err
			}
			cfg.Email = strings.TrimSpace(line)
		}
		if strings.TrimSpace(*password) == "" {
			secret, err := promptSecret("Bambu password: ")
			if err != nil {
				return err
			}
			*password = secret
		}
		login, err := bambuLogin(cfg.APIBase, cfg.Email, *password)
		if err != nil {
			return err
		}
		cfg.AccessToken = login.AccessToken
		cfg.RefreshToken = login.RefreshToken
		if login.ExpiresIn > 0 {
			cfg.TokenExpiresAt = time.Now().Add(time.Duration(login.ExpiresIn) * time.Second).Unix()
		}
	}

	if cfg.AccessToken == "" {
		return errors.New("Bambu login did not return an access token")
	}

	client := newBambuHTTPClient(cfg.APIBase, cfg.AccessToken)
	userID, err := client.userID()
	if err != nil {
		return fmt.Errorf("load Bambu user id: %w", err)
	}
	cfg.UserID = userID

	devices, err := client.devices()
	if err != nil {
		return fmt.Errorf("load Bambu devices: %w", err)
	}
	device, err := chooseBambuDevice(devices, cfg.DeviceID)
	if err != nil {
		return err
	}
	cfg.DeviceID = device.ID
	cfg.DeviceName = device.Name
	cfg.DeviceModel = device.Model
	cfg.DeviceProduct = device.ProductName
	cfg.LastLoginUnixSec = time.Now().Unix()

	if err := saveBambuConfig(*configPath, cfg); err != nil {
		return err
	}

	fmt.Printf("Saved Bambu config for %s (%s) to %s\n", device.Name, device.ID, *configPath)
	return nil
}

type bambuLoginResult struct {
	AccessToken  string
	RefreshToken string
	ExpiresIn    int64
}

func bambuLogin(apiBase, email, password string) (bambuLoginResult, error) {
	result, response, err := bambuLoginRequest(apiBase, map[string]any{
		"account":  email,
		"password": password,
		"apiError": "",
	})
	if err != nil {
		return result, err
	}
	if result.AccessToken != "" {
		return result, nil
	}

	body := responseBody(response)
	loginType := stringFromMap(body, "loginType")
	switch loginType {
	case "verifyCode":
		if err := bambuSendEmailCode(apiBase, email); err != nil {
			return result, err
		}
		code, err := promptLine("Email verification code: ")
		if err != nil {
			return result, err
		}
		result, _, err = bambuLoginRequest(apiBase, map[string]any{
			"account": email,
			"code":    strings.TrimSpace(code),
		})
		return result, err
	case "tfa":
		code, err := promptLine("Bambu 2FA code: ")
		if err != nil {
			return result, err
		}
		tfaKey := stringFromMap(body, "tfaKey")
		return bambuTFALogin(apiBase, tfaKey, strings.TrimSpace(code))
	default:
		if msg := firstNonEmpty(stringFromMap(body, "message"), stringFromMap(response, "message")); msg != "" {
			return result, fmt.Errorf("Bambu login failed: %s", msg)
		}
		return result, fmt.Errorf("Bambu login failed: missing token, loginType=%q", loginType)
	}
}

func bambuSendEmailCode(apiBase, email string) error {
	_, err := postBambuJSON(apiBase+"/v1/user-service/user/sendemail/code", "", map[string]any{
		"email": email,
		"type":  "codeLogin",
	})
	return err
}

func bambuLoginRequest(apiBase string, body map[string]any) (bambuLoginResult, map[string]any, error) {
	var result bambuLoginResult
	response, err := postBambuJSON(apiBase+"/v1/user-service/user/login", "", body)
	if err != nil {
		return result, response, err
	}
	result = parseBambuLoginResult(response)
	return result, response, nil
}

func bambuTFALogin(apiBase, tfaKey, code string) (bambuLoginResult, error) {
	var result bambuLoginResult
	response, err := postBambuJSON(apiBase+"/api/sign-in/tfa", "", map[string]any{
		"tfaKey":  tfaKey,
		"tfaCode": code,
	})
	if err != nil {
		return result, err
	}
	result = parseBambuLoginResult(response)
	if result.AccessToken == "" {
		return result, errors.New("Bambu TFA did not return an access token")
	}
	return result, nil
}

func parseBambuLoginResult(response map[string]any) bambuLoginResult {
	body := responseBody(response)
	return bambuLoginResult{
		AccessToken:  stringFromMap(body, "accessToken"),
		RefreshToken: stringFromMap(body, "refreshToken"),
		ExpiresIn:    int64FromMap(body, "expiresIn"),
	}
}

type bambuHTTPClient struct {
	apiBase string
	token   string
	client  *http.Client
}

func newBambuHTTPClient(apiBase, token string) bambuHTTPClient {
	return bambuHTTPClient{
		apiBase: apiBase,
		token:   token,
		client:  &http.Client{Timeout: 30 * time.Second},
	}
}

func (c bambuHTTPClient) userID() (string, error) {
	response, err := c.get("/v1/design-user-service/my/preference")
	if err != nil {
		return "", err
	}
	body := responseBody(response)
	uid := stringFromMap(body, "uid")
	if uid == "" {
		return "", errors.New("Bambu preference response did not include uid")
	}
	return strings.TrimPrefix(uid, "u_"), nil
}

func (c bambuHTTPClient) devices() ([]bambuDevice, error) {
	response, err := c.get("/v1/iot-service/api/user/bind")
	if err != nil {
		return nil, err
	}
	devices := parseBambuDevices(responseBody(response))
	if len(devices) == 0 {
		devices = parseBambuDevices(response)
	}
	if len(devices) == 0 {
		return nil, errors.New("Bambu account has no bound devices")
	}
	return devices, nil
}

func (c bambuHTTPClient) get(path string) (map[string]any, error) {
	request, err := http.NewRequest(http.MethodGet, c.apiBase+path, nil)
	if err != nil {
		return nil, err
	}
	request.Header.Set("Authorization", "Bearer "+c.token)
	request.Header.Set("Accept", "application/json")
	return doBambuRequest(c.client, request)
}

func postBambuJSON(url, token string, body map[string]any) (map[string]any, error) {
	data, err := json.Marshal(body)
	if err != nil {
		return nil, err
	}
	request, err := http.NewRequest(http.MethodPost, url, bytes.NewReader(data))
	if err != nil {
		return nil, err
	}
	request.Header.Set("Content-Type", "application/json")
	request.Header.Set("Accept", "application/json")
	if token != "" {
		request.Header.Set("Authorization", "Bearer "+token)
	}
	return doBambuRequest(&http.Client{Timeout: 30 * time.Second}, request)
}

func doBambuRequest(client *http.Client, request *http.Request) (map[string]any, error) {
	response, err := client.Do(request)
	if err != nil {
		return nil, err
	}
	defer response.Body.Close()

	data, err := io.ReadAll(io.LimitReader(response.Body, 2<<20))
	if err != nil {
		return nil, err
	}
	if response.StatusCode < 200 || response.StatusCode > 299 {
		return nil, fmt.Errorf("%s returned HTTP %d: %s", request.URL.Host, response.StatusCode, strings.TrimSpace(string(data)))
	}
	if len(bytes.TrimSpace(data)) == 0 {
		return map[string]any{}, nil
	}
	var out map[string]any
	if err := json.Unmarshal(data, &out); err != nil {
		return nil, err
	}
	return out, nil
}

func startPrinterStatusLoop() {
	if envBool("AGENT_VIEWER_BAMBU_SIM") {
		go simulatePrinterLoop()
		return
	}
	go bambuCloudLoop()
}

func bambuCloudLoop() {
	retryDelay := printerReconnectMin
	for {
		started := time.Now()
		if err := runBambuCloudMonitor(); err != nil {
			fmt.Printf("[bambu] %v\n", err)
			if time.Since(started) >= printerStableRunReset {
				retryDelay = printerReconnectMin
			}
			sendPrinterStatusIfChanged(offlinePrinterStatus(bambuOfflineJob(err)))
			fmt.Printf("[bambu] reconnecting in %s\n", retryDelay)
			time.Sleep(retryDelay)
			retryDelay = nextPrinterReconnectDelay(retryDelay)
		}
	}
}

func bambuOfflineJob(err error) string {
	if err == nil {
		return "Bambu Cloud disconnected"
	}
	msg := strings.ToLower(err.Error())
	if strings.Contains(msg, "config missing") || strings.Contains(msg, "bambu-login") {
		return "Run bambu-login"
	}
	return "Bambu Cloud disconnected"
}

func nextPrinterReconnectDelay(delay time.Duration) time.Duration {
	if delay <= 0 {
		return printerReconnectMin
	}
	delay *= 2
	if delay > printerReconnectMax {
		return printerReconnectMax
	}
	return delay
}

func offlinePrinterStatus(job string) printerStatus {
	return printerStatus{
		State:           "offline",
		ProgressPercent: -1,
		EtaSeconds:      -1,
		Layer:           -1,
		Layers:          -1,
		Job:             job,
		Source:          "Cloud",
	}
}

func runBambuCloudMonitor() error {
	cfg, err := loadBambuRuntimeConfig()
	if err != nil {
		return err
	}

	sendPrinterStatus(printerStatus{
		State:           "offline",
		ProgressPercent: -1,
		EtaSeconds:      -1,
		Layer:           -1,
		Layers:          -1,
		Job:             "Connecting to Bambu Cloud",
		Source:          "Cloud",
	})

	reportTopic := "device/" + cfg.DeviceID + "/report"
	requestTopic := "device/" + cfg.DeviceID + "/request"
	brokerURL := "ssl://" + cfg.MQTTBroker
	host, _, _ := net.SplitHostPort(cfg.MQTTBroker)
	if host == "" {
		host = cfg.MQTTBroker
	}

	var printMu sync.Mutex
	printData := map[string]any{}
	lastUpdate := time.Now()
	staleReported := false
	done := make(chan error, 1)

	options := mqtt.NewClientOptions()
	options.AddBroker(brokerURL)
	options.SetClientID(fmt.Sprintf("agent-viewer-%d-%d", os.Getpid(), time.Now().Unix()))
	options.SetUsername("u_" + strings.TrimPrefix(cfg.UserID, "u_"))
	options.SetPassword(cfg.AccessToken)
	options.SetKeepAlive(60 * time.Second)
	options.SetConnectTimeout(20 * time.Second)
	options.SetTLSConfig(&tls.Config{MinVersion: tls.VersionTLS12, ServerName: host})
	options.SetConnectionLostHandler(func(client mqtt.Client, err error) {
		select {
		case done <- err:
		default:
		}
	})
	options.SetDefaultPublishHandler(func(client mqtt.Client, message mqtt.Message) {
		var payload map[string]any
		if err := json.Unmarshal(message.Payload(), &payload); err != nil {
			fmt.Printf("[bambu] invalid MQTT JSON: %v\n", err)
			return
		}
		print, ok := mapFromAny(payload["print"])
		if !ok {
			return
		}
		printMu.Lock()
		mergeJSONMap(printData, print)
		clearStaleBambuErrorFields(printData, print)
		status := printerStatusFromBambuPrint(printData)
		status.Source = "Cloud"
		ams, hasAMS := amsStatusFromBambuPrint(printData)
		lastUpdate = time.Now()
		staleReported = false
		printMu.Unlock()
		sendPrinterStatus(status)
		if hasAMS {
			sendAMSStatusIfChanged(ams)
		}
	})
	options.SetOnConnectHandler(func(client mqtt.Client) {
		if token := client.Subscribe(reportTopic, 0, nil); token.Wait() && token.Error() != nil {
			fmt.Printf("[bambu] subscribe failed: %v\n", token.Error())
			return
		}
		publishBambuPushAll(client, requestTopic)
		fmt.Printf("[bambu] subscribed to %s\n", reportTopic)
	})

	client := mqtt.NewClient(options)
	if token := client.Connect(); !token.WaitTimeout(25 * time.Second) {
		return errors.New("Bambu MQTT connect timeout")
	} else if token.Error() != nil {
		return fmt.Errorf("Bambu MQTT connect failed: %w", token.Error())
	}
	defer client.Disconnect(250)

	ticker := time.NewTicker(printerSendInterval)
	defer ticker.Stop()

	for {
		select {
		case err := <-done:
			return fmt.Errorf("Bambu MQTT disconnected: %w", err)
		case <-ticker.C:
			printMu.Lock()
			stale := time.Since(lastUpdate) > printerStaleAfter
			reportStale := stale && !staleReported
			if reportStale {
				staleReported = true
			}
			printMu.Unlock()
			if reportStale {
				sendPrinterStatus(offlinePrinterStatus("Bambu Cloud stale"))
			}
			publishBambuPushAll(client, requestTopic)
		}
	}
}

func publishBambuPushAll(client mqtt.Client, topic string) {
	payload := map[string]any{
		"pushing": map[string]any{
			"sequence_id": strconv.FormatInt(time.Now().Unix(), 10),
			"command":     "pushall",
			"version":     1,
			"push_target": 1,
		},
	}
	data, _ := json.Marshal(payload)
	if token := client.Publish(topic, 0, false, data); token.WaitTimeout(5*time.Second) && token.Error() != nil {
		fmt.Printf("[bambu] pushall failed: %v\n", token.Error())
	}
}

func simulatePrinterLoop() {
	samples := []printerStatus{
		{State: "idle", ProgressPercent: -1, EtaSeconds: -1, Layer: -1, Layers: -1, Job: "Ready", Material: "PLA", Source: "Sim"},
		{State: "printing", ProgressPercent: 18, EtaSeconds: 6840, Layer: 12, Layers: 120, NozzleC: "220", BedC: "60", ChamberC: "35", Job: "clock-bezel.3mf", Material: "PLA", Source: "Sim"},
		{State: "printing", ProgressPercent: 64, EtaSeconds: 2460, Layer: 77, Layers: 120, NozzleC: "219", BedC: "60", ChamberC: "36", Job: "clock-bezel.3mf", Material: "PLA", Source: "Sim"},
		{State: "paused", ProgressPercent: 64, EtaSeconds: 2460, Layer: 77, Layers: 120, NozzleC: "195", BedC: "55", ChamberC: "34", Job: "clock-bezel.3mf", Material: "PLA", Source: "Sim"},
		{State: "error", ProgressPercent: 64, EtaSeconds: -1, Layer: 77, Layers: 120, NozzleC: "180", BedC: "50", ChamberC: "32", Job: "AMS filament check", Material: "PLA", Source: "Sim"},
	}
	ams := amsStatus{
		ActiveSlot: 0,
		Humidity:   "2",
		Trays: [amsTrayCount]amsTrayStatus{
			{Material: "PLA", Color: "22C55E", RemainingPercent: 96},
			{Material: "PETG", Color: "38BDF8", RemainingPercent: 64},
			{Material: "ABS", Color: "F97316", RemainingPercent: 28},
			{Material: "", Color: "", RemainingPercent: -1},
		},
	}
	i := 0
	for {
		sendPrinterStatus(samples[i%len(samples)])
		ams.ActiveSlot = i % amsTrayCount
		sendAMSStatus(ams)
		i++
		time.Sleep(printerSendInterval)
	}
}

func sendPrinterStatus(status printerStatus) {
	normalizePrinterStatus(&status)

	printerMu.Lock()
	currentPrinter = status
	printerMu.Unlock()

	sendMultiPayload(formatPrinterPayload(status))
}

func sendPrinterStatusIfChanged(status printerStatus) bool {
	normalizePrinterStatus(&status)

	printerMu.Lock()
	if samePrinterStatusView(currentPrinter, status) {
		printerMu.Unlock()
		return false
	}
	currentPrinter = status
	printerMu.Unlock()

	return sendMultiPayload(formatPrinterPayload(status))
}

func normalizePrinterStatus(status *printerStatus) {
	if status.Updated.IsZero() {
		status.Updated = time.Now()
	}
	if status.State == "" {
		status.State = "unknown"
	}
	if status.Source == "" {
		status.Source = "Cloud"
	}
}

func samePrinterStatusView(a, b printerStatus) bool {
	return a.State == b.State &&
		a.ProgressPercent == b.ProgressPercent &&
		a.EtaSeconds == b.EtaSeconds &&
		a.Layer == b.Layer &&
		a.Layers == b.Layers &&
		a.NozzleC == b.NozzleC &&
		a.BedC == b.BedC &&
		a.ChamberC == b.ChamberC &&
		a.Job == b.Job &&
		a.Material == b.Material &&
		a.Source == b.Source
}

func sendCurrentPrinterStatus() {
	printerMu.Lock()
	status := currentPrinter
	printerMu.Unlock()
	if status.State == "" {
		return
	}
	sendMultiPayload(formatPrinterPayload(status))
}

func sendAMSStatus(status amsStatus) {
	normalizeAMSStatus(&status)

	printerMu.Lock()
	currentAMS = status
	currentAMSValid = true
	printerMu.Unlock()

	sendMultiPayload(formatAMSPayload(status))
}

func sendAMSStatusIfChanged(status amsStatus) bool {
	normalizeAMSStatus(&status)

	printerMu.Lock()
	if currentAMSValid && sameAMSStatusView(currentAMS, status) {
		printerMu.Unlock()
		return false
	}
	currentAMS = status
	currentAMSValid = true
	printerMu.Unlock()

	return sendMultiPayload(formatAMSPayload(status))
}

func sendCurrentAMSStatus() {
	printerMu.Lock()
	status := currentAMS
	valid := currentAMSValid
	printerMu.Unlock()
	if !valid || status.Updated.IsZero() {
		return
	}
	sendMultiPayload(formatAMSPayload(status))
}

func normalizeAMSStatus(status *amsStatus) {
	if status.Updated.IsZero() {
		status.Updated = time.Now()
	}
	if status.ActiveSlot < 0 || status.ActiveSlot >= amsTrayCount {
		status.ActiveSlot = -1
	}
}

func sameAMSStatusView(a, b amsStatus) bool {
	if a.ActiveSlot != b.ActiveSlot || a.Humidity != b.Humidity {
		return false
	}
	for i := 0; i < amsTrayCount; i++ {
		if a.Trays[i] != b.Trays[i] {
			return false
		}
	}
	return true
}

func formatPrinterPayload(status printerStatus) string {
	fields := []string{
		"P",
		cleanField(status.State, maxPrinterStateLen),
		formatOptionalInt(status.ProgressPercent),
		formatOptionalInt(status.EtaSeconds),
		formatOptionalInt(status.Layer),
		formatOptionalInt(status.Layers),
		cleanField(status.NozzleC, maxPrinterTempLen),
		cleanField(status.BedC, maxPrinterTempLen),
		cleanField(status.ChamberC, maxPrinterTempLen),
		cleanField(status.Job, maxPrinterJobLen),
		cleanField(status.Material, maxPrinterMaterialLen),
		cleanField(status.Source, maxPrinterSourceLen),
		strconv.FormatInt(status.Updated.UnixMilli(), 10),
	}
	return strings.Join(fields, "\t")
}

func formatAMSPayload(status amsStatus) string {
	fields := []string{
		"A",
		formatOptionalInt(status.ActiveSlot),
		cleanField(status.Humidity, maxAMSHumidityLen),
	}
	for i := 0; i < amsTrayCount; i++ {
		fields = append(fields,
			cleanField(status.Trays[i].Material, maxAMSMaterialLen),
			cleanField(status.Trays[i].Color, maxAMSColorLen),
			formatOptionalInt(status.Trays[i].RemainingPercent),
		)
	}
	fields = append(fields, strconv.FormatInt(status.Updated.UnixMilli(), 10))
	return strings.Join(fields, "\t")
}

func formatOptionalInt(value int) string {
	if value < 0 {
		return ""
	}
	return strconv.Itoa(value)
}

func amsStatusFromBambuPrint(print map[string]any) (amsStatus, bool) {
	status := amsStatus{ActiveSlot: -1}

	if ams, ok := mapFromAny(print["ams"]); ok {
		status.Humidity = firstNonEmpty(
			stringFromMap(ams, "humidity"),
			stringFromMap(ams, "humidity_raw"),
			stringFromMap(ams, "humidity_level"),
			stringFromMap(ams, "humidity_percent"),
			stringFromMap(ams, "ams_humidity"),
			stringFromMap(ams, "ams_humidity_percent"),
		)

		trayNow := intFromMap(ams, "tray_now", -1)
		if trayNow >= 0 && trayNow < amsTrayCount {
			status.ActiveSlot = trayNow
		} else if trayNow >= 0 && trayNow < 255 && trayNow/amsTrayCount == 0 {
			status.ActiveSlot = trayNow % amsTrayCount
		}

		if fillAMSFromUnits(&status, ams) {
			return status, true
		}
	}

	if fillAMSFromVirSlot(&status, print) {
		return status, true
	}

	return status, false
}

func fillAMSFromUnits(status *amsStatus, ams map[string]any) bool {
	units, ok := ams["ams"].([]any)
	if !ok {
		return false
	}

	var selected map[string]any
	for _, item := range units {
		unit, ok := mapFromAny(item)
		if !ok {
			continue
		}
		if selected == nil {
			selected = unit
		}
		if intFromMap(unit, "id", 0) == 0 {
			selected = unit
			break
		}
	}
	if selected == nil {
		return false
	}

	trays, ok := selected["tray"].([]any)
	if !ok {
		return false
	}
	found := false
	for _, trayItem := range trays {
		tray, ok := mapFromAny(trayItem)
		if !ok {
			continue
		}
		slot := intFromMap(tray, "id", -1)
		if slot < 0 || slot >= amsTrayCount {
			continue
		}
		status.Trays[slot] = amsTrayStatus{
			Material: firstNonEmpty(
				stringFromMap(tray, "tray_type"),
				stringFromMap(tray, "tray_sub_brands"),
			),
			Color:            normalizeAMSColor(stringFromMap(tray, "tray_color")),
			RemainingPercent: remainingPercentFromMap(tray),
		}
		found = true
	}
	return found
}

func fillAMSFromVirSlot(status *amsStatus, print map[string]any) bool {
	slots, ok := print["vir_slot"].([]any)
	if !ok {
		return false
	}
	found := false
	for _, item := range slots {
		slotMap, ok := mapFromAny(item)
		if !ok {
			continue
		}
		slot := intFromMap(slotMap, "id", -1)
		if slot < 0 || slot >= amsTrayCount {
			continue
		}
		status.Trays[slot] = amsTrayStatus{
			Material:         stringFromMap(slotMap, "tray_type"),
			Color:            normalizeAMSColor(stringFromMap(slotMap, "tray_color")),
			RemainingPercent: remainingPercentFromMap(slotMap),
		}
		found = true
	}
	return found
}

func remainingPercentFromMap(values map[string]any) int {
	for _, key := range []string{
		"remain",
		"remaining",
		"remaining_percent",
		"remain_percent",
		"tray_remain",
		"tray_remaining",
		"percent",
		"filament_percent",
	} {
		value := intFromMap(values, key, -1)
		if value >= 0 && value <= 100 {
			return value
		}
	}
	return -1
}

func normalizeAMSColor(color string) string {
	color = strings.TrimSpace(strings.TrimPrefix(color, "#"))
	if len(color) > maxAMSColorLen {
		color = color[:maxAMSColorLen]
	}
	for _, r := range color {
		if !((r >= '0' && r <= '9') || (r >= 'a' && r <= 'f') || (r >= 'A' && r <= 'F')) {
			return ""
		}
	}
	return strings.ToUpper(color)
}

func printerStatusFromBambuPrint(print map[string]any) printerStatus {
	progress := intFromMap(print, "mc_percent", -1)
	status := printerStatus{
		State:           normalizeBambuPrintState(print, progress),
		ProgressPercent: progress,
		EtaSeconds:      remainingSecondsFromBambu(print),
		Layer:           intFromMap(print, "layer_num", -1),
		Layers:          intFromMap(print, "total_layer_num", -1),
		NozzleC:         temperatureFromMap(print, "nozzle_temper"),
		BedC:            temperatureFromMap(print, "bed_temper"),
		ChamberC:        temperatureFromMap(print, "chamber_temper"),
		Job:             bambuJobName(print),
		Material:        bambuMaterial(print),
		Source:          "Cloud",
	}
	if status.ProgressPercent > 100 {
		status.ProgressPercent = 100
	}
	return status
}

func normalizeBambuPrintState(print map[string]any, progress int) string {
	state := strings.ToUpper(strings.TrimSpace(stringFromMap(print, "gcode_state")))
	switch state {
	case "RUNNING", "PREPARE", "PREPARING", "SLICING":
		return "printing"
	case "PAUSE", "PAUSED":
		if hasBambuError(print) {
			return "error"
		}
		return "paused"
	case "FAILED", "FAIL", "ERROR":
		return "error"
	case "FINISH", "FINISHED", "SUCCESS", "IDLE":
		return "idle"
	}
	if hasBambuError(print) {
		return "error"
	}
	if progress > 0 && progress < 100 {
		return "printing"
	}
	if state == "" {
		return "unknown"
	}
	return strings.ToLower(state)
}

func hasBambuError(print map[string]any) bool {
	if intFromMap(print, "print_error", 0) != 0 {
		return true
	}
	if intFromMap(print, "mc_print_error_code", 0) != 0 {
		return true
	}
	if hms, ok := print["hms"].([]any); ok && len(hms) > 0 {
		return true
	}
	return false
}

func clearStaleBambuErrorFields(merged, update map[string]any) {
	state := strings.ToUpper(strings.TrimSpace(stringFromMap(update, "gcode_state")))
	switch state {
	case "RUNNING", "PREPARE", "PREPARING", "SLICING", "FINISH", "FINISHED", "SUCCESS", "IDLE":
	default:
		return
	}

	if _, ok := update["print_error"]; !ok {
		delete(merged, "print_error")
	}
	if _, ok := update["mc_print_error_code"]; !ok {
		delete(merged, "mc_print_error_code")
	}
	if _, ok := update["hms"]; !ok {
		delete(merged, "hms")
	}
}

func remainingSecondsFromBambu(print map[string]any) int {
	if minutes := intFromMap(print, "mc_remaining_time", -1); minutes >= 0 {
		return minutes * 60
	}
	if minutes := intFromMap(print, "remaining_minutes", -1); minutes >= 0 {
		return minutes * 60
	}
	if seconds := intFromMap(print, "remaining_seconds", -1); seconds >= 0 {
		return seconds
	}
	if seconds := intFromMap(print, "remaining_time", -1); seconds >= 0 {
		return seconds
	}
	return -1
}

func bambuJobName(print map[string]any) string {
	for _, key := range []string{"subtask_name", "job_name", "gcode_file", "file", "project_name"} {
		if value := strings.TrimSpace(stringFromMap(print, key)); value != "" {
			value = strings.ReplaceAll(value, "\\", "/")
			if strings.Contains(value, "/") {
				value = filepath.Base(value)
			}
			return value
		}
	}
	return ""
}

func bambuMaterial(print map[string]any) string {
	if vtTray, ok := mapFromAny(print["vt_tray"]); ok {
		if material := stringFromMap(vtTray, "tray_type"); material != "" {
			return material
		}
	}

	if ams, ok := mapFromAny(print["ams"]); ok {
		trayNow := intFromMap(ams, "tray_now", -1)
		if trayNow >= 0 && trayNow < 255 {
			if material := materialFromAMS(ams, trayNow); material != "" {
				return material
			}
		}
		if material := firstMaterialFromAMS(ams); material != "" {
			return material
		}
	}

	if material := materialFromVirSlot(print, -1); material != "" {
		return material
	}
	return ""
}

func materialFromAMS(ams map[string]any, trayNow int) string {
	units, ok := ams["ams"].([]any)
	if !ok {
		return ""
	}
	unitID := trayNow / 4
	slotID := trayNow % 4
	for _, item := range units {
		unit, ok := mapFromAny(item)
		if !ok {
			continue
		}
		if id := intFromMap(unit, "id", unitID); id != unitID {
			continue
		}
		trays, ok := unit["tray"].([]any)
		if !ok {
			continue
		}
		for _, trayItem := range trays {
			tray, ok := mapFromAny(trayItem)
			if !ok || intFromMap(tray, "id", -1) != slotID {
				continue
			}
			return stringFromMap(tray, "tray_type")
		}
	}
	return ""
}

func firstMaterialFromAMS(ams map[string]any) string {
	units, ok := ams["ams"].([]any)
	if !ok {
		return ""
	}
	for _, item := range units {
		unit, ok := mapFromAny(item)
		if !ok {
			continue
		}
		trays, ok := unit["tray"].([]any)
		if !ok {
			continue
		}
		for _, trayItem := range trays {
			tray, ok := mapFromAny(trayItem)
			if !ok {
				continue
			}
			if material := stringFromMap(tray, "tray_type"); material != "" {
				return material
			}
		}
	}
	return ""
}

func materialFromVirSlot(print map[string]any, trayNow int) string {
	slots, ok := print["vir_slot"].([]any)
	if !ok {
		return ""
	}
	var first string
	for _, item := range slots {
		slot, ok := mapFromAny(item)
		if !ok {
			continue
		}
		material := stringFromMap(slot, "tray_type")
		if material == "" {
			continue
		}
		if first == "" {
			first = material
		}
		if trayNow >= 0 && intFromMap(slot, "id", -1) == trayNow {
			return material
		}
	}
	return first
}

func mergeJSONMap(dst, src map[string]any) {
	for key, value := range src {
		if srcMap, ok := mapFromAny(value); ok {
			if dstMap, ok := mapFromAny(dst[key]); ok {
				mergeJSONMap(dstMap, srcMap)
				dst[key] = dstMap
				continue
			}
		}
		dst[key] = value
	}
}

func temperatureFromMap(values map[string]any, key string) string {
	value, ok := floatFromAny(values[key])
	if !ok {
		return ""
	}
	if math.Abs(value-math.Round(value)) < 0.05 {
		return fmt.Sprintf("%.0f", value)
	}
	return fmt.Sprintf("%.1f", value)
}

func parseBambuDevices(body map[string]any) []bambuDevice {
	raw, ok := body["devices"].([]any)
	if !ok {
		raw, ok = body["data"].([]any)
	}
	if !ok {
		return nil
	}
	devices := make([]bambuDevice, 0, len(raw))
	for _, item := range raw {
		m, ok := mapFromAny(item)
		if !ok {
			continue
		}
		id := stringFromMap(m, "dev_id")
		if id == "" {
			id = stringFromMap(m, "device_id")
		}
		if id == "" {
			continue
		}
		devices = append(devices, bambuDevice{
			ID:          id,
			Name:        firstNonEmpty(stringFromMap(m, "name"), stringFromMap(m, "dev_name"), id),
			Model:       stringFromMap(m, "dev_model_name"),
			ProductName: stringFromMap(m, "dev_product_name"),
			Online:      boolFromMap(m, "online"),
		})
	}
	return devices
}

func chooseBambuDevice(devices []bambuDevice, requested string) (bambuDevice, error) {
	requested = strings.TrimSpace(requested)
	if requested != "" {
		for _, device := range devices {
			if device.ID == requested {
				return device, nil
			}
		}
		return bambuDevice{}, fmt.Errorf("Bambu device %q was not found", requested)
	}

	var p2s []bambuDevice
	for _, device := range devices {
		text := strings.ToLower(device.Name + " " + device.Model + " " + device.ProductName)
		if strings.Contains(text, "p2s") || strings.Contains(text, "p2") {
			p2s = append(p2s, device)
		}
	}
	if len(p2s) == 1 {
		return p2s[0], nil
	}
	if len(devices) == 1 {
		return devices[0], nil
	}

	fmt.Println("Bambu devices:")
	for i, device := range devices {
		fmt.Printf("  %d. %s (%s, %s)\n", i+1, device.Name, device.ID, firstNonEmpty(device.ProductName, device.Model, "unknown model"))
	}
	line, err := promptLine("Select printer number: ")
	if err != nil {
		return bambuDevice{}, err
	}
	index, err := strconv.Atoi(strings.TrimSpace(line))
	if err != nil || index < 1 || index > len(devices) {
		return bambuDevice{}, errors.New("invalid printer selection")
	}
	return devices[index-1], nil
}

func loadBambuRuntimeConfig() (bambuConfig, error) {
	cfg, err := readBambuConfig(bambuConfigPath())
	if err != nil && !errors.Is(err, os.ErrNotExist) {
		return cfg, err
	}
	applyBambuEnv(&cfg)
	if cfg.Region == "" {
		cfg.Region = "global"
	}
	if cfg.APIBase == "" {
		cfg.APIBase = bambuAPIBase(cfg.Region)
	}
	if cfg.MQTTBroker == "" {
		cfg.MQTTBroker = bambuMQTTBroker(cfg.Region)
	}
	if cfg.AccessToken == "" || cfg.UserID == "" || cfg.DeviceID == "" {
		return cfg, fmt.Errorf("Bambu config missing; run `agent-viewer bambu-login` or set AGENT_VIEWER_BAMBU_TOKEN, AGENT_VIEWER_BAMBU_USER_ID, and AGENT_VIEWER_BAMBU_DEVICE_ID")
	}
	return cfg, nil
}

func readBambuConfig(path string) (bambuConfig, error) {
	var cfg bambuConfig
	data, err := os.ReadFile(path)
	if err != nil {
		return cfg, err
	}
	if err := json.Unmarshal(data, &cfg); err != nil {
		return cfg, err
	}
	return cfg, nil
}

func saveBambuConfig(path string, cfg bambuConfig) error {
	data, err := json.MarshalIndent(cfg, "", "  ")
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		return err
	}
	return os.WriteFile(path, append(data, '\n'), 0600)
}

func applyBambuEnv(cfg *bambuConfig) {
	if value := strings.TrimSpace(os.Getenv("AGENT_VIEWER_BAMBU_REGION")); value != "" {
		cfg.Region = normalizeBambuRegion(value)
	}
	if value := strings.TrimSpace(os.Getenv("AGENT_VIEWER_BAMBU_API")); value != "" {
		cfg.APIBase = value
	}
	if value := strings.TrimSpace(os.Getenv("AGENT_VIEWER_BAMBU_MQTT")); value != "" {
		cfg.MQTTBroker = value
	}
	if value := strings.TrimSpace(os.Getenv("AGENT_VIEWER_BAMBU_TOKEN")); value != "" {
		cfg.AccessToken = value
	}
	if value := strings.TrimSpace(os.Getenv("AGENT_VIEWER_BAMBU_USER_ID")); value != "" {
		cfg.UserID = strings.TrimPrefix(value, "u_")
	}
	if value := strings.TrimSpace(os.Getenv("AGENT_VIEWER_BAMBU_DEVICE_ID")); value != "" {
		cfg.DeviceID = value
	}
}

func bambuConfigPath() string {
	if path := strings.TrimSpace(os.Getenv("AGENT_VIEWER_BAMBU_CONFIG")); path != "" {
		return expandHome(path)
	}
	base, err := os.UserConfigDir()
	if err != nil || base == "" {
		home, _ := os.UserHomeDir()
		base = filepath.Join(home, ".config")
	}
	return filepath.Join(base, "agent-viewer", "bambu.json")
}

func bambuAPIBase(region string) string {
	if normalizeBambuRegion(region) == "china" {
		return "https://api.bambulab.cn"
	}
	return "https://api.bambulab.com"
}

func bambuMQTTBroker(region string) string {
	if normalizeBambuRegion(region) == "china" {
		return "cn.mqtt.bambulab.com:8883"
	}
	return "us.mqtt.bambulab.com:8883"
}

func normalizeBambuRegion(region string) string {
	region = strings.ToLower(strings.TrimSpace(region))
	if region == "cn" || region == "china" {
		return "china"
	}
	return "global"
}

func responseBody(response map[string]any) map[string]any {
	for _, key := range []string{"data", "body"} {
		if body, ok := mapFromAny(response[key]); ok {
			return body
		}
	}
	return response
}

func mapFromAny(value any) (map[string]any, bool) {
	m, ok := value.(map[string]any)
	return m, ok
}

func stringFromMap(values map[string]any, key string) string {
	value, ok := values[key]
	if !ok || value == nil {
		return ""
	}
	switch v := value.(type) {
	case string:
		return strings.TrimSpace(v)
	case json.Number:
		return strings.TrimSpace(v.String())
	case float64:
		if math.Trunc(v) == v {
			return strconv.FormatInt(int64(v), 10)
		}
		return strconv.FormatFloat(v, 'f', -1, 64)
	case bool:
		return strconv.FormatBool(v)
	default:
		return strings.TrimSpace(fmt.Sprint(v))
	}
}

func intFromMap(values map[string]any, key string, fallback int) int {
	value, ok := values[key]
	if !ok {
		return fallback
	}
	if number, ok := intFromAny(value); ok {
		return number
	}
	return fallback
}

func int64FromMap(values map[string]any, key string) int64 {
	value, ok := values[key]
	if !ok {
		return 0
	}
	if number, ok := intFromAny(value); ok {
		return int64(number)
	}
	return 0
}

func intFromAny(value any) (int, bool) {
	switch v := value.(type) {
	case int:
		return v, true
	case int64:
		return int(v), true
	case float64:
		return int(v), true
	case json.Number:
		i, err := strconv.Atoi(v.String())
		if err == nil {
			return i, true
		}
	case string:
		v = strings.TrimSpace(v)
		if v == "" {
			return 0, false
		}
		i, err := strconv.Atoi(v)
		if err == nil {
			return i, true
		}
	}
	return 0, false
}

func floatFromAny(value any) (float64, bool) {
	switch v := value.(type) {
	case float64:
		return v, true
	case float32:
		return float64(v), true
	case int:
		return float64(v), true
	case json.Number:
		f, err := strconv.ParseFloat(v.String(), 64)
		return f, err == nil
	case string:
		f, err := strconv.ParseFloat(strings.TrimSpace(v), 64)
		return f, err == nil
	}
	return 0, false
}

func boolFromMap(values map[string]any, key string) bool {
	value, ok := values[key]
	if !ok {
		return false
	}
	switch v := value.(type) {
	case bool:
		return v
	case string:
		return strings.EqualFold(v, "true") || v == "1"
	default:
		return false
	}
}

func promptLine(prompt string) (string, error) {
	fmt.Print(prompt)
	reader := bufio.NewReader(os.Stdin)
	line, err := reader.ReadString('\n')
	if err != nil && !errors.Is(err, io.EOF) {
		return "", err
	}
	return strings.TrimSpace(line), nil
}

func promptSecret(prompt string) (string, error) {
	fmt.Print(prompt)
	fd := int(os.Stdin.Fd())
	if term.IsTerminal(fd) {
		data, err := term.ReadPassword(fd)
		fmt.Println()
		if err != nil {
			return "", err
		}
		return strings.TrimSpace(string(data)), nil
	}
	return promptLine("")
}

func envDefault(key, fallback string) string {
	if value := strings.TrimSpace(os.Getenv(key)); value != "" {
		return value
	}
	return fallback
}

func envBool(key string) bool {
	value := strings.ToLower(strings.TrimSpace(os.Getenv(key)))
	return value == "1" || value == "true" || value == "yes" || value == "on"
}

func firstNonEmpty(values ...string) string {
	for _, value := range values {
		if strings.TrimSpace(value) != "" {
			return strings.TrimSpace(value)
		}
	}
	return ""
}
