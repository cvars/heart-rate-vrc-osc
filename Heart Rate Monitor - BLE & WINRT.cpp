#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <iostream>
#include <unordered_set>
#include <map>
#include <functional>
#include <thread>
#include <atomic>
#include <chrono> // For timing
#include <lo/lo.h> // OSC library

using namespace winrt;
using namespace winrt::Windows::Devices::Bluetooth::Advertisement;
using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace winrt::Windows::Storage::Streams;

class BluetoothLEManager {
public:
    BluetoothLEManager() {
        watcher.ScanningMode(BluetoothLEScanningMode::Active);
        onHeartRateMeasurementReceived = std::bind(&BluetoothLEManager::PrintHeartRateMeasurement, this, std::placeholders::_1, std::placeholders::_2);

        // Initialize OSC address for sending heart rate measurements
        oscAddress = createOSCAddress("127.0.0.1", "9000"); // Example: Send to localhost, port 9000
    }

    // Function to create and return an OSC address
    lo_address createOSCAddress(const char* destination, const char* port) {
        lo_address address = lo_address_new(destination, port);
        if (!address) {
            // Handle error if address creation failed
            fprintf(stderr, "Error creating OSC address\n");
            exit(1);
        }
        return address;
    }

    ~BluetoothLEManager() {
        // Free OSC address when done
        lo_address_free(oscAddress);
    }

    void StartScanning() {
        StartWatcher();
    }

    void StopScanning() {
        StopWatcher();
    }

    void ConnectToDevice(int index) {
        if (indexedDevices.count(index)) {
            auto connectedDevice = Connect(indexedDevices.at(index));
            if (connectedDevice) {
                SubscribeToHeartRateMeasurement(connectedDevice);
            }
            else {
                std::wcout << L"Failed to connect to the device." << std::endl;
            }
        }
        else {
            std::wcout << L"Invalid index selected." << std::endl;
        }
    }

    void StopSubscription() {
        continueRunning.store(false);
    }

private:
    std::function<void(GattCharacteristic, GattValueChangedEventArgs)> onHeartRateMeasurementReceived;

    BluetoothLEAdvertisementWatcher watcher;
    std::unordered_set<uint64_t> uniqueDevices;
    std::map<int, uint64_t> indexedDevices;
    int deviceIndex = 1;
    std::atomic<bool> continueRunning{ true };

    lo_address oscAddress; // OSC address for sending messages

    // Static variables to track previous heart rate and time of last sent measurement
    static uint16_t previousHeartRate;
    static std::chrono::steady_clock::time_point lastSentTime;

    void StartWatcher() {
        watcher.Received([&](const auto&, const BluetoothLEAdvertisementReceivedEventArgs& args) {
            HandleAdvertisement(args);
            });

        watcher.Start();
        std::wcout << L"Scanning for devices. Press Enter to stop scanning." << std::endl;
    }

    void StopWatcher() {
        std::wstring input;
        auto start = std::chrono::steady_clock::now(); // Get current time

        // Wait for 1 second or until user input is received
        while (std::chrono::steady_clock::now() - start < std::chrono::seconds(1) && std::wcin.peek() == L'\n') {
            std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Sleep for 100 milliseconds
        }

        if (std::wcin.peek() == L'\n') {
            std::getline(std::wcin, input); // Consume the newline character
        }

        watcher.Stop();
    }

    void HandleAdvertisement(const BluetoothLEAdvertisementReceivedEventArgs& args) {
        auto deviceAddress = args.BluetoothAddress();
        if (uniqueDevices.insert(deviceAddress).second) {
            indexedDevices[deviceIndex] = deviceAddress;
            auto localName = args.Advertisement().LocalName();
            localName = localName.empty() ? L"Unknown" : localName;
            std::wcout << L"[" << deviceIndex << L"] Device found: " << localName.c_str() << L" (" << deviceAddress << L")" << std::endl;
            deviceIndex++;
        }
    }

    BluetoothLEDevice Connect(uint64_t bluetoothAddress) {
        BluetoothLEDevice bleDevice = nullptr;
        try {
            auto bleDeviceOperation = BluetoothLEDevice::FromBluetoothAddressAsync(bluetoothAddress).get();
            if (bleDeviceOperation) {
                std::wcout << L"Connected to device: " << bleDeviceOperation.DeviceId().c_str() << std::endl;
                bleDevice = bleDeviceOperation;
            }
            else {
                std::wcout << L"Failed to connect to device." << std::endl;
            }
        }
        catch (const std::exception& e) {
            std::cerr << "Exception: " << e.what() << std::endl;
        }
        return bleDevice;
    }

    void PrintHeartRateMeasurement(const GattCharacteristic&, const GattValueChangedEventArgs& args)
    {
        DataReader reader = DataReader::FromBuffer(args.CharacteristicValue());
        if (reader.UnconsumedBufferLength() > 0)
        {
            uint8_t flags = reader.ReadByte();
            uint16_t heartRateValue = 0;

            if (flags & 0x01) {
                heartRateValue = reader.ReadUInt16();
            }
            else {
                heartRateValue = reader.ReadByte();
            }

            // Send heart rate measurement via OSC only if it's different and every 3 seconds
            auto currentTime = std::chrono::steady_clock::now();
            auto timeElapsed = std::chrono::duration_cast<std::chrono::seconds>(currentTime - lastSentTime).count();

            if (heartRateValue != previousHeartRate /*&& timeElapsed >= 1.5*/) {

                std::wcout << L"Heart Rate Measurement: " << heartRateValue << L" bpm" << std::endl;
                sendHeartRateMeasurementOSC(heartRateValue);
                previousHeartRate = heartRateValue;
                lastSentTime = currentTime;
            }
        }
    }

    // Function to send heart rate measurement via OSC
    void sendHeartRateMeasurementOSC(uint16_t heartRate) {
        // Example OSC path for heart rate measurement
        const char* hr_path = "/chatbox/input";

        // Convert heart rate to a string
        char heartRateStr[32];  // Make sure it's large enough to hold the number
        sprintf_s(heartRateStr, sizeof(heartRateStr), "Heart Rate %d", heartRate);

        // Send OSC message
        lo_send(oscAddress, hr_path, "s", heartRateStr);
    }

    void SubscribeToHeartRateMeasurement(BluetoothLEDevice& device) {
        auto hrServiceUuid = BluetoothUuidHelper::FromShortId(0x180D);
        auto hrMeasurementCharUuid = BluetoothUuidHelper::FromShortId(0x2A37);

        auto hrServiceResult = device.GetGattServicesForUuidAsync(hrServiceUuid).get();
        if (hrServiceResult.Status() != GattCommunicationStatus::Success) {
            std::wcout << L"Failed to find Heart Rate service." << std::endl;
            return;
        }

        auto hrService = hrServiceResult.Services().GetAt(0);
        auto hrCharResult = hrService.GetCharacteristicsForUuidAsync(hrMeasurementCharUuid).get();
        if (hrCharResult.Status() != GattCommunicationStatus::Success) {
            std::wcout << L"Failed to find Heart Rate Measurement characteristic." << std::endl;
            return;
        }

        auto hrChar = hrCharResult.Characteristics().GetAt(0);
        hrChar.ValueChanged(onHeartRateMeasurementReceived);

        auto status = hrChar.WriteClientCharacteristicConfigurationDescriptorAsync(GattClientCharacteristicConfigurationDescriptorValue::Notify).get();
        if (status != GattCommunicationStatus::Success) {
            std::wcout << L"Failed to subscribe to Heart Rate Measurement notifications." << std::endl;
        }
        else {
            std::wcout << L"Subscribed to Heart Rate Measurement notifications." << std::endl;
        }

        // Event loop with a check every 1 second
        while (continueRunning.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
};

// Static member initialization
uint16_t BluetoothLEManager::previousHeartRate = 0;
std::chrono::steady_clock::time_point BluetoothLEManager::lastSentTime = std::chrono::steady_clock::now();

int main() {
    init_apartment();

    BluetoothLEManager manager;
    manager.StartScanning();
    manager.StopScanning();

    std::wcout << L"Select a device to connect (enter index): ";
    int selectedIndex;
    std::wcin >> selectedIndex;
    manager.ConnectToDevice(selectedIndex);

    return 0;
}
