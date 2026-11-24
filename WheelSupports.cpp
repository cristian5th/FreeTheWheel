//
//  WheelSupports.cpp
//  WheelSupportTools
//
//  Created by Annop Prapasapong on 27/11/2012.
//  Copyright (c) 2012 Feral Interactive. All rights reserved.
//

#include <unistd.h>
#include <CoreFoundation/CFString.h>
#include <string>
#include <cstdlib>
#include "WheelSupports.h"

// Keep the configuration manager and native DFGT references alive for the process lifetime.
// The discovery manager is separate and opens shared, so it cannot seize the working input path.
static IOHIDManagerRef gHIDManagerRef = NULL;
static IOHIDManagerRef gNativeDFGTDiscoveryManager = NULL;
static IOHIDDeviceRef gNativeDFGTDevice = NULL;
static bool gHIDManagerOpen = false;
static IOReturn gHIDManagerOpenResult = kIOReturnNotReady;
static IOReturn gNativeDFGTDiscoveryOpenResult = kIOReturnNotReady;
static bool gNativeDFGTDeviceOpen = false;


//=============================================================================
// Mode strings
const char *sGPLogitechModeRestricted = "RESTRICTED";
const char *sGPLogitechModeNative = "NATIVE";


//=============================================================================
//		SetApplierFunctionCopyToCFArray : Applier function for CFSetApplyFunction
//-----------------------------------------------------------------------------
void SetApplierFunctionCopyToCFArray(const void *value, void *context)
{
    CFArrayAppendValue((CFMutableArrayRef) context, value);
}



//=============================================================================
//		AllocateHIDManager
//-----------------------------------------------------------------------------
IOHIDManagerRef AllocateHIDManager()
{
    if (gHIDManagerRef) return gHIDManagerRef;

    gHIDManagerRef = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDManagerOptionNone);
    if (!gHIDManagerRef) return NULL;

    IOHIDManagerSetDeviceMatching(gHIDManagerRef, NULL);
    // Preserve the original FreeTheWheel behaviour: on this HID stack,
    // manager seize may report exclusive access even though the original
    // per-device enumeration/configuration path can still work.
    gHIDManagerOpenResult = IOHIDManagerOpen(gHIDManagerRef, kIOHIDOptionsTypeSeizeDevice);
    gHIDManagerOpen = (gHIDManagerOpenResult == kIOReturnSuccess);
    if (!gHIDManagerOpen) {
        printf("Warning: IOHIDManagerOpen returned (%x); continuing with device enumeration.\n",
               (unsigned int)gHIDManagerOpenResult);
    }
    return gHIDManagerRef;
}

static void NativeDFGTDeviceMatched(void *, IOReturn result, void *,
                                    IOHIDDeviceRef device)
{
    if (result != kIOReturnSuccess || !device || gNativeDFGTDevice) return;

    UInt32 vendor = GetPropertyNumber(device, CFSTR(kIOHIDVendorIDKey));
    UInt32 product = GetPropertyNumber(device, CFSTR(kIOHIDProductIDKey));
    if (vendor == 0x046d && product == 0xc29a) {
        gNativeDFGTDevice = (IOHIDDeviceRef)CFRetain(device);
    }
}

static void RetainNativeDFGTIfPresent()
{
    if (gNativeDFGTDevice) return;

    // The original configuration manager may still hold the pre-re-enumeration
    // C294 device list. Discover C29A with a fresh, non-seizing manager instead.
    if (!gNativeDFGTDiscoveryManager) {
        gNativeDFGTDiscoveryManager =
            IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDManagerOptionNone);
        if (!gNativeDFGTDiscoveryManager) {
            printf("Warning: could not create native DFGT discovery manager.\n");
            return;
        }

        int vendorID = 0x046d;
        int productID = 0xc29a;
        CFNumberRef vendorNumber = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vendorID);
        CFNumberRef productNumber = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &productID);
        CFMutableDictionaryRef matching = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        if (!vendorNumber || !productNumber || !matching) {
            if (vendorNumber) CFRelease(vendorNumber);
            if (productNumber) CFRelease(productNumber);
            if (matching) CFRelease(matching);
            printf("Warning: could not create native DFGT matching dictionary.\n");
            return;
        }

        CFDictionarySetValue(matching, CFSTR(kIOHIDVendorIDKey), vendorNumber);
        CFDictionarySetValue(matching, CFSTR(kIOHIDProductIDKey), productNumber);
        IOHIDManagerSetDeviceMatching(gNativeDFGTDiscoveryManager, matching);
        CFRelease(matching);
        CFRelease(vendorNumber);
        CFRelease(productNumber);

        // Register and schedule before opening so macOS can deliver the
        // matching callback when the wheel reappears as native PID C29A.
        IOHIDManagerRegisterDeviceMatchingCallback(
            gNativeDFGTDiscoveryManager, NativeDFGTDeviceMatched, nullptr);
        IOHIDManagerScheduleWithRunLoop(
            gNativeDFGTDiscoveryManager, CFRunLoopGetCurrent(),
            kCFRunLoopDefaultMode);

        gNativeDFGTDiscoveryOpenResult =
            IOHIDManagerOpen(gNativeDFGTDiscoveryManager, kIOHIDOptionsTypeNone);
        if (gNativeDFGTDiscoveryOpenResult != kIOReturnSuccess) {
            printf("Warning: shared DFGT discovery manager open returned (%x).\n",
                   (unsigned int)gNativeDFGTDiscoveryOpenResult);
            IOHIDManagerUnscheduleFromRunLoop(
                gNativeDFGTDiscoveryManager, CFRunLoopGetCurrent(),
                kCFRunLoopDefaultMode);
            return;
        }
        printf("Opened scheduled shared DFGT discovery manager.\n");
    }

    // Allow up to 10 seconds for macOS to publish the re-enumerated C29A.
    // Run the current loop so IOHIDManager's device-matching callback executes.
    for (int attempt = 0; attempt < 100 && !gNativeDFGTDevice; ++attempt) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true);

        // Snapshot fallback in case the device was enumerated before callback
        // delivery was serviced.
        CFSetRef devices = IOHIDManagerCopyDevices(gNativeDFGTDiscoveryManager);
        if (devices) {
            CFIndex count = CFSetGetCount(devices);
            const void **values = (const void **)calloc((size_t)count, sizeof(void *));
            if (values) {
                CFSetGetValues(devices, values);
                for (CFIndex i = 0; i < count; ++i) {
                    IOHIDDeviceRef device = (IOHIDDeviceRef)values[i];
                    UInt32 vendor = GetPropertyNumber(device, CFSTR(kIOHIDVendorIDKey));
                    UInt32 product = GetPropertyNumber(device, CFSTR(kIOHIDProductIDKey));
                    if (vendor == 0x046d && product == 0xc29a) {
                        gNativeDFGTDevice = (IOHIDDeviceRef)CFRetain(device);
                        break;
                    }
                }
                free(values);
            }
            CFRelease(devices);
        }
    }

    if (gNativeDFGTDevice)
        printf("Retained native Logitech DFGT HID reference (VID 046D, PID C29A).\n");
    else
        printf("Warning: native DFGT HID reference was not retained after 10 seconds.\n");
}
bool IsNativeDFGTAvailable()
{
    return gNativeDFGTDevice != NULL;
}

bool IsHIDManagerOpen()
{
    return gHIDManagerOpen;
}

IOReturn HIDManagerOpenResult()
{
    return gHIDManagerOpenResult;
}

bool IsNativeDFGTDeviceOpen()
{
    return gNativeDFGTDeviceOpen;
}

IOReturn OpenNativeDFGTDevice()
{
    if (!gNativeDFGTDevice) return kIOReturnNoDevice;
    if (gNativeDFGTDeviceOpen) return kIOReturnSuccess;
    return OpenDevice(gNativeDFGTDevice);
}

IOReturn OpenNativeDFGTDeviceShared()
{
    if (!gNativeDFGTDevice) return kIOReturnNoDevice;
    if (gNativeDFGTDeviceOpen) return kIOReturnSuccess;

    // Shared open keeps the system's normal HID input path available.
    IOReturn result = IOHIDDeviceOpen(gNativeDFGTDevice, kIOHIDOptionsTypeNone);
    if (result == kIOReturnSuccess) gNativeDFGTDeviceOpen = true;
    return result;
}

IOReturn SendNativeDFGTOutputReport(const UInt8 *report, CFIndex length)
{
    if (!gNativeDFGTDevice || !report) return kIOReturnBadArgument;
    if (!gNativeDFGTDeviceOpen) return kIOReturnNotReady;
    if (length != 7) return kIOReturnBadArgument;

    // Native DFGT C29A exposes a seven-byte output report without a Report ID.
    return IOHIDDeviceSetReport(gNativeDFGTDevice, kIOHIDReportTypeOutput,
                                0, report, length);
}

IOReturn CloseNativeDFGTDevice()
{
    if (!gNativeDFGTDevice) return kIOReturnNoDevice;
    if (!gNativeDFGTDeviceOpen) return kIOReturnSuccess;
    return CloseDevice(gNativeDFGTDevice);
}



//=============================================================================
//		ConfigAllDevices : Scan connected devices and apply early configs as need
//										 Return true if any change is made
//-----------------------------------------------------------------------------
bool ConfigAllDevices(const DeviceMode mode)
{
	bool changed = false;
    int tryCount = 0;

    while(changed == false && tryCount < MAXIMUM_TRIES)
    {
        tryCount++;

        // Obtain a copy of the list of connected devices
        IOHIDManagerRef managerRef = AllocateHIDManager();
        if (!managerRef) return false;
        CFSetRef deviceCFSetRef = IOHIDManagerCopyDevices(managerRef);
        if (!deviceCFSetRef) {
            usleep(100000);
            continue;
        }

        CFIndex count = CFSetGetCount(deviceCFSetRef);
        CFArrayRef devCFArrayRef = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
        CFSetApplyFunction(deviceCFSetRef, SetApplierFunctionCopyToCFArray, (void*)devCFArrayRef);

        // Going through the list, determining their ID and call the appropriate function;
        for(CFIndex i = 0; i < count; i++)
        {
            IOHIDDeviceRef hidDevice = (IOHIDDeviceRef) CFArrayGetValueAtIndex(devCFArrayRef, i);

            UInt32 vendorID = GetPropertyNumber(hidDevice, CFSTR(kIOHIDVendorIDKey));
            UInt32 productID = GetPropertyNumber(hidDevice, CFSTR(kIOHIDProductIDKey));
            DeviceID deviceID = MakeDeviceID(productID, vendorID);

            changed |= ConfigDevice(hidDevice, deviceID, mode);
        }

        // Release the list
        CFRelease(devCFArrayRef);
        CFRelease(deviceCFSetRef);
    }

    if (mode == DeviceModeFull && changed) RetainNativeDFGTIfPresent();

    printf("Succeeded after %i tries.", tryCount);

	return changed;
}



//=============================================================================
//		ConfigDevice : Config a device using current settngs
//-----------------------------------------------------------------------------
bool ConfigDevice(IOHIDDeviceRef hidDevice, DeviceID deviceID, const DeviceMode mode)
{
    
	switch(deviceID)
	{
		case kGPLogitechWheelRestricted:
			return ConfigLogitechWheels(hidDevice, deviceID, false, mode);
			break;			
		case kGPLogitechG25Native:
            printf("Logitech G25 Native mode enabled.\n");
            return ConfigLogitechWheels(hidDevice, deviceID, true, mode);
		case kGPLogitechG27Native:
            printf("Logitech G27 Native mode enabled.\n");
            return ConfigLogitechWheels(hidDevice, deviceID, true, mode);
		case kGPLogitechG29Native:
            printf("Logitech G29 Native mode enabled.\n");
            return ConfigLogitechWheels(hidDevice, deviceID, true, mode);
		case kGPLogitechDFGTNative:
            printf("Logitech Driving Force GT Native mode enabled.\n");
            return ConfigLogitechWheels(hidDevice, deviceID, true, mode);
		case kGPLogitechDFPNative:
            printf("Logitech Driving Force Pro Native mode enabled.\n");
			return ConfigLogitechWheels(hidDevice, deviceID, true, mode);
		case kGPLogitechG920Native:
			printf("Logitech G920 Native mode enabled.\n");
			return ConfigLogitechWheels(hidDevice, deviceID, true, mode);
		default:
			return false;
	}
}



//=============================================================================
//		OpenDevice : Open given device if it is not opened yet
//-----------------------------------------------------------------------------
IOReturn OpenDevice(IOHIDDeviceRef hidDevice)
{
    if (!hidDevice) return kIOReturnBadArgument;

    IOReturn result = IOHIDDeviceOpen(hidDevice, kIOHIDOptionsTypeSeizeDevice);
    if (result != kIOReturnSuccess) {
        const char *reason = "IOKit error";
        switch (result) {
            case kIOReturnError: reason = "general error"; break;
            case kIOReturnNoMemory: reason = "can't allocate memory"; break;
            case kIOReturnNoResources: reason = "resource shortage"; break;
            case kIOReturnIPCError: reason = "error during IPC"; break;
            case kIOReturnNoDevice: reason = "no such device"; break;
            case kIOReturnNotPrivileged: reason = "privilege violation"; break;
            case kIOReturnBadArgument: reason = "invalid argument"; break;
            case kIOReturnLockedRead: reason = "device read locked"; break;
            case kIOReturnLockedWrite: reason = "device write locked"; break;
            case kIOReturnExclusiveAccess: reason = "exclusive access"; break;
            case kIOReturnBadMessageID: reason = "bad message ID"; break;
            case kIOReturnUnsupported: reason = "unsupported function"; break;
            case kIOReturnVMError: reason = "VM failure"; break;
        }
        printf("Error: OpenDevice failed - %s - (%x)\n", reason, (unsigned int)result);
        return result;
    }

    if (hidDevice == gNativeDFGTDevice) gNativeDFGTDeviceOpen = true;
    return kIOReturnSuccess;
}





//=============================================================================
//		CloseDevice : Close given device if it is opened
//-----------------------------------------------------------------------------
IOReturn CloseDevice(IOHIDDeviceRef hidDevice)
{
    if (!hidDevice) return kIOReturnBadArgument;
    IOReturn result = IOHIDDeviceClose(hidDevice, 0);
    if (result == kIOReturnSuccess && hidDevice == gNativeDFGTDevice)
        gNativeDFGTDeviceOpen = false;
    return result;
}





//=============================================================================
//		SendCommands : Send given commands to the device
//-----------------------------------------------------------------------------
IOReturn SendCommands(IOHIDDeviceRef hidDevice, CCommands *commands)
{
	for(int i=0; i<commands->count; ++i)
	{
		UInt8 *cmd = commands->cmds[i];
		IOReturn result = IOHIDDeviceSetReport(hidDevice, kIOHIDReportTypeOutput, time(NULL), cmd, kGPCommandMaxLength);
		if (result != kIOReturnSuccess)
		{
			printf("WARNING: SendCommand failed with result: %x\n", result);
			return result;
		}
	}
	return kIOReturnSuccess;
}



//=============================================================================
//		GetPropertyString : Obtain the property string data of the device
//-----------------------------------------------------------------------------
CFStringRef GetPropertyString(IOHIDDeviceRef hidDevice, CFStringRef property)
{
	// Obtain the data as untyped pointer and convert it to CString
	CFTypeRef dataRef = IOHIDDeviceGetProperty(hidDevice, property);
	return CFStringCreateCopy(kCFAllocatorDefault, CFStringRef(dataRef));
}



//=============================================================================
//		GetPropertyString : Obtain the property number data of the device
//-----------------------------------------------------------------------------
UInt32 GetPropertyNumber(IOHIDDeviceRef hidDevice, CFStringRef property)
{
	// Obtain the data as untyped pointer and convert it to CString
	CFTypeRef dataRef = IOHIDDeviceGetProperty(hidDevice, property);
	if(dataRef && (CFNumberGetTypeID() == CFGetTypeID(dataRef)))
	{
		UInt32 number;
		CFNumberGetValue((CFNumberRef)dataRef, kCFNumberSInt32Type, &number);
		return number;
	}
	return 0;
}



//=============================================================================
//		ConfigLogitechWheels : for Logitech wheels
//-----------------------------------------------------------------------------
bool ConfigLogitechWheels(IOHIDDeviceRef hidDevice, DeviceID deviceID, bool native, const DeviceMode targetMode)
{
	if(targetMode == DeviceModeInfoOnly)
	{
		CFStringRef productID = GetPropertyString(hidDevice, CFSTR(kIOHIDProductKey));
		const char *mode = native ? sGPLogitechModeNative : sGPLogitechModeRestricted;

		char sProductID[256];
		CFStringGetCString(productID, sProductID, 256, kCFStringEncodingASCII);

		printf("Device ID=%x   Product ID=%s (%s)\n", deviceID, sProductID, mode);
		return false;
	}
	
	if(!native)
	{
		// Don't do anything is the feature is not enabled
		if(targetMode == DeviceModeStandard)
		{
			return false;
		}
		
		// Determine proper native device ID from the device's Product ID string
		// As the restricted device ID are the same for all 4 supported devices
		CFStringRef productID = GetPropertyString(hidDevice, CFSTR(kIOHIDProductKey));
		DeviceID targetDeviceID;
		if(CFStringFind(productID, CFSTR(kGPLogitechG25ProductID), 0).location == 0)
		{
			targetDeviceID = kGPLogitechG25Native;
            printf("Logitech G25 Native mode enabled.\n");
		}
		else if(CFStringFind(productID, CFSTR(kGPLogitechG27ProductID), 0).location == 0)
		{
			targetDeviceID = kGPLogitechG27Native;
            printf("Logitech G27 Native mode enabled.\n");
		}
		else if(CFStringFind(productID, CFSTR(kGPLogitechG29ProductID), 0).location == 0)
		{
			targetDeviceID = kGPLogitechG29Native;
            printf("Logitech G29 Native mode enabled.\n");
		}
		else if(CFStringFind(productID, CFSTR(kGPLogitechDFGTProductID), 0).location == 0)
		{
			targetDeviceID = kGPLogitechDFGTNative;
            printf("Logitech Driving Force GT Native mode enabled.\n");
		}
		else if(CFStringFind(productID, CFSTR(kGPLogitechDFPProductID), 0).location == 0)
		{
			targetDeviceID = kGPLogitechDFPNative;
            printf("Logitech Driving Force Pro Native mode enabled.\n");
		}
		else if(CFStringFind(productID, CFSTR(kGPLogitechG920ProductID), 0).location == 0)
		{
			targetDeviceID = kGPLogitechG920Native;
			printf("Logitech G920 Native mode enabled.\n");
		}
		else return false;
		
		// Activate full native and 900 degree mode
		if(OpenDevice(hidDevice) == kIOReturnSuccess)
		{
            usleep(DELAY_BETWEEN_COMMANDS);
			CCommands commands;
			GetCmdLogitechWheelRange(&commands, targetDeviceID, kGPLogitechWheelRangeMax);
			if (SendCommands(hidDevice, &commands) != kIOReturnSuccess)
				return false;
			printf("Calibrated full wheel range. (VendorID/DeviceID %x)\n", deviceID);
			
            usleep(DELAY_BETWEEN_COMMANDS);
			GetCmdLogitechWheelNative(&commands, targetDeviceID);
			if (SendCommands(hidDevice, &commands) != kIOReturnSuccess)
				return false;
			printf("Enabled native mode. (VendorID/DeviceID %x)\n", deviceID);
			
                     
			CloseDevice(hidDevice);
			return true;
		}
	}
	else
	{
		if(OpenDevice(hidDevice) == kIOReturnSuccess)
		{
			CCommands commands;
			bool changed = false;
			
			if(targetMode == DeviceModeFull)
			{
				// Activate full 900 degree mode, as being in native mode doesn't guarantee the wheel range
				GetCmdLogitechWheelRange(&commands, deviceID, kGPLogitechWheelRangeMax);
				SendCommands(hidDevice, &commands);
				
                printf("Calibrated 900 degree wheel movement. (VendorID/DeviceID %x)\n", deviceID);
				changed = true;
			}
			else
			{
				// We don't know how to go back to restricted mode, but we can set the wheel back to 240 degree range
				GetCmdLogitechWheelRange(&commands, deviceID, kGPLogitechWheelRangeStandard);
				SendCommands(hidDevice, &commands);
				
                printf("Reset device to default 320 degree wheel movement. (VendorID/DeviceID %x)\n", deviceID);
				changed = true;
			}
			
			CloseDevice(hidDevice);
			return changed;
		}
	}
	
	return false;
}



//=============================================================================
//		GetCmdLogitechWheelNative : Activate native mode for given device ID
//-----------------------------------------------------------------------------
void GetCmdLogitechWheelNative(CCommands *c, const DeviceID deviceID)
{
	// Fill the command array with null
	memset(c->cmds, 0, kGPCommandsDataSize);
	
	// Fill in the command value according to target deviceID
	switch (deviceID) {
		case kGPLogitechG25Native:
			c->cmds[0][0] = 0xf8;
			c->cmds[0][1] = 0x10;
			c->count = 1;
			break;
		case kGPLogitechG27Native:
		case kGPLogitechG29Native:


            // Original commands
//			c->cmds[0][0] = 0xf8;
//			c->cmds[0][1] = 0x0a;
//			c->cmds[1][0] = 0xf8;
//			c->cmds[1][1] = 0x09;
//			c->cmds[1][2] = 0x04;
//			c->cmds[1][3] = 0x01;
//			c->count = 1;
            
            // https://github.com/TripleSpeeder/LTWheelConf/blob/master/wheels.c
            
            c->cmds[0][0] = 0xf8;
            c->cmds[0][1] = 0x0a;
            c->cmds[0][2] = 0x00;
            c->cmds[0][3] = 0x00;
            c->cmds[0][4] = 0x00;
            c->cmds[0][5] = 0x00;
            c->cmds[0][6] = 0x00;
            c->cmds[0][7] = 0x00;

            // Partial button mapping
            c->cmds[1][0] = 0xf8;
            c->cmds[1][1] = 0x01;
            c->cmds[1][2] = 0x00;
            c->cmds[1][3] = 0x00;
            c->cmds[1][4] = 0x00;
            c->cmds[1][5] = 0x00;
            c->cmds[1][6] = 0x00;
            c->cmds[1][7] = 0x00;
            
            // Full button mapping with clutch
//            c->cmds[1][0] = 0xf8;
//            c->cmds[1][1] = 0x09;
//            c->cmds[1][2] = 0x04;
//            c->cmds[1][3] = 0x01;
//            c->cmds[1][4] = 0x00;
//            c->cmds[1][5] = 0x00;
//            c->cmds[1][6] = 0x00;
//            c->cmds[1][7] = 0x00;

			c->count = 2;
            
			break;
		case kGPLogitechDFGTNative:
			c->cmds[0][0] = 0xf8;
			c->cmds[0][1] = 0x0a;
			c->cmds[1][0] = 0xf8;
			c->cmds[1][1] = 0x09;
			c->cmds[1][2] = 0x03;
			c->cmds[1][3] = 0x01;
			c->count = 2;
			break;
		case kGPLogitechDFPNative:
			c->cmds[0][0] = 0xf8;
			c->cmds[0][1] = 0x01;
			c->count = 1;
			break;
		case kGPLogitechG920Native:
			c->cmds[0][0] = 0xf8;
			c->cmds[0][1] = 0x0a;
			c->cmds[0][2] = 0x00;
			c->cmds[0][3] = 0x00;
			c->cmds[0][4] = 0x00;
			c->cmds[0][5] = 0x00;
			c->cmds[0][6] = 0x00;
			c->cmds[0][7] = 0x00;

			c->count = 2;
			break;
		default:
			c->count = 0;
			break;
	}
}



//=============================================================================
//		GetCmdLogitechWheelRange : Set wheel range for given device ID
//-----------------------------------------------------------------------------
void GetCmdLogitechWheelRange(CCommands *c, const DeviceID deviceID, int range)
{
	// Fill the command array with null
	memset(c->cmds, 0, kGPCommandsDataSize);
	
	// Fill in the command value according to target deviceID
	switch (deviceID) {
		case kGPLogitechG25Native:
		case kGPLogitechG27Native:
		case kGPLogitechG29Native:
		case kGPLogitechDFGTNative:
		{
			c->cmds[0][0] = 0xf8;
			c->cmds[0][1] = 0x81;
			c->cmds[0][2] = (range & 0x00ff);
			c->cmds[0][3] = (range & 0xff00) >> 8;
			c->cmds[0][4] = 0x00;
			c->cmds[0][5] = 0x00;
			c->cmds[0][6] = 0x00;
			c->cmds[0][7] = 0x00;
            
//            c->cmds[1][0] = 0xfe;
//            c->cmds[1][1] = 0x0d;
//            c->cmds[1][2] = 0 & 0x0f;
//            c->cmds[1][3] = 0 & 0x0f;
//            c->cmds[1][4] = 0 & 0xff;
//            c->cmds[1][5] = 0x00;
//            c->cmds[1][6] = 0x00;
//            c->cmds[1][7] = 0x00;
            
			c->count = 1;
			break;
		}
		case kGPLogitechDFPNative:
		{
			c->cmds[0][0] = 0xf8;
			c->cmds[1][0] = 0x81;
			c->cmds[1][1] = 0x0b;
			c->count = 2;
			
			// Determine the needs for range limiter
			int rampLeft;
			int rampRight;
			int fullRange = 0;
			
			if (range > 200)
			{
				c->cmds[0][1] = 0x03;
				fullRange = 900;
			}
			else
			{
				c->cmds[0][1] = 0x02;
				fullRange = 200;
			}
			
			// If target range less than full range; Apply limiter command
			if (range < fullRange)
			{
				rampLeft = (((fullRange - range + 1) * 2047) / fullRange);
				rampRight = 0xfff - rampLeft;
				
				c->cmds[1][2] = rampLeft >> 4;
				c->cmds[1][3] = rampRight >> 4;
				c->cmds[1][4] = 0xff;
				c->cmds[1][5] = (rampRight & 0xe) << 4 | (rampLeft & 0xe);
				c->cmds[1][6] = 0xff;
			}
			break;
		}
		case kGPLogitechG920Native:
		{
			c->cmds[0][0] = 0xf8;
			c->cmds[0][1] = 0x61;
			c->cmds[0][2] = (range & 0x00ff);
			c->cmds[0][3] = (range & 0xff00) >> 8;
			c->cmds[0][4] = 0x00;
			c->cmds[0][5] = 0x00;
			c->cmds[0][6] = 0x00;
			c->cmds[0][7] = 0x00;
		}
		default:
			c->count = 0;
			break;
	}
}
