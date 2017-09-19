/*
 * Copyright (c) 1998-2000 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.1 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 *
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

#include <IOKit/assert.h>
#include <IOKit/IOService.h>
#include "../IOSyncer.h"
#include "ApplePS2KeyboardDevice.h"
#include "ApplePS2MouseDevice.h"
#include "ApplePS2Controller.h"

extern "C"
{
    //#include <pexpert/i386/protos.h>
#include <machine/machine_routines.h>
}

//#warning FIXME: use inb and outb from the kernel framework (2688371)
typedef unsigned short i386_ioport_t;
inline unsigned char inb(i386_ioport_t port)
{
    unsigned char datum;
    asm volatile("inb %1, %0" : "=a" (datum) : "d" (port));
    return(datum);
}

inline void outb(i386_ioport_t port, unsigned char datum)
{
    asm volatile("outb %0, %1" : : "a" (datum), "d" (port));
}

enum {
    kPS2PowerStateSleep  = 0,
    kPS2PowerStateDoze   = 1,
    kPS2PowerStateNormal = 2,
    kPS2PowerStateCount
};

static const IOPMPowerState PS2PowerStateArray[ kPS2PowerStateCount ] =
{
    { 1,0,0,0,0,0,0,0,0,0,0,0 },
    { 1,kIOPMDeviceUsable, kIOPMDoze, kIOPMDoze, 0,0,0,0,0,0,0,0 },
    { 1,kIOPMDeviceUsable, IOPMPowerOn, IOPMPowerOn, 0,0,0,0,0,0,0,0 }
};

static ApplePS2Controller * gApplePS2Controller = 0;  // global variable to self

// =============================================================================
// Interrupt-Time Support Functions
//

static void interruptHandlerMouse(OSObject *, void *, IOService *, int)
{
    //
    // Wake our workloop to service the interrupt.    This is an edge-triggered
    // interrupt, so returning from this routine without clearing the interrupt
    // condition is perfectly normal.
    //
    
    gApplePS2Controller->_interruptSourceMouse->interruptOccurred(0, 0, 0);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void interruptHandlerKeyboard(OSObject *, void *, IOService *, int)
{
    //
    // Wake our workloop to service the interrupt.    This is an edge-triggered
    // interrupt, so returning from this routine without clearing the interrupt
    // condition is perfectly normal.
    //
    
    gApplePS2Controller->_interruptSourceKeyboard->interruptOccurred(0, 0, 0);
    
}

// =============================================================================
// ApplePS2Controller Class Implementation
//

#define super IOService
OSDefineMetaClassAndStructors(ApplePS2Controller, IOService);

bool ApplePS2Controller::init(OSDictionary * properties)
{
    if (!super::init(properties))  return false;
    
    //
    // Initialize minimal state.
    //
    
    _workLoop                = 0;
    
    _interruptSourceKeyboard = 0;
    _interruptSourceMouse    = 0;
    
    _interruptTargetKeyboard = 0;
    _interruptTargetMouse    = 0;
    
    _interruptActionKeyboard = NULL;
    _interruptActionMouse    = NULL;
    
    _interruptInstalledKeyboard = false;
    _interruptInstalledMouse    = false;
    
    //PS2 Notification
    _ps2notificationActionKeyboard = NULL;
    _ps2notificationActionMouseTouchpad = NULL;
    
    _ps2notificationTargetKeyboard = 0;
    _ps2notificationTargetMouseTouchpad = 0;
    
    _ps2notificationInstalledKeyboard = false;
    _ps2notifiationInstalledMouseTouchpad = false;
    //
    _mouseDevice    = 0;
    _keyboardDevice = 0;
    
    queue_init(&_requestQueue);
    
    _currentPowerState = kPS2PowerStateNormal;
    
    return true;
}

void ApplePS2Controller::free(void)
{
    super::free();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

bool ApplePS2Controller::start(IOService * provider)
{
    //
    // The driver has been instructed to start.  Allocate all our resources.
    //
    if (!super::start(provider))  return false;
    
    //
    // Initialize the mouse and keyboard hardware to a known state --  the IRQs
    // are disabled (don't want interrupts), the clock line is enabled (want to
    // be able to send commands), and the device itself is disabled (don't want
    // asynchronous data arrival for key/mouse events).  We call the read/write
    // port routines directly, since no other thread will conflict with us.
    //
    
    UInt8 commandByte;
    writeCommandPort(kCP_GetCommandByte);
    commandByte  =  readDataPort(kDT_Keyboard);
    commandByte &= ~(kCB_EnableMouseIRQ | kCB_DisableMouseClock);
    writeCommandPort(kCP_SetCommandByte);
    writeDataPort(commandByte);
    
    writeDataPort(kDP_SetDefaultsAndDisable);
    readDataPort(kDT_Keyboard);       // (discard acknowledge; success irrelevant)
    
    writeCommandPort(kCP_TransmitToMouse);
    writeDataPort(kDP_SetDefaultsAndDisable);
    readDataPort(kDT_Mouse);          // (discard acknowledge; success irrelevant)
    
    //
    // Clear out garbage in the controller's input streams, before starting up
    // the work loop.
    //
    
    while ( inb(kCommandPort) & kOutputReady )
    {
        IODelay(kDataDelay);
        inb(kDataPort);
        IODelay(kDataDelay);
    }
    
    //
    // Use a spin lock to protect the client async request queue.
    //
    
    _requestQueueLock = IOSimpleLockAlloc();
    if (!_requestQueueLock) goto fail;
    
    //
    // Initialize our work loop, our command gate, and our interrupt event
    // sources.  The work loop can accept requests after this step.
    //
    
    _workLoop                = IOWorkLoop::workLoop();
   
    _interruptSourceMouse    = IOInterruptEventSource::interruptEventSource( this,
                                                                            OSMemberFunctionCast(IOInterruptEventAction, this, &ApplePS2Controller::interruptOccurred));
	_interruptSourceKeyboard = IOInterruptEventSource::interruptEventSource( this,
                                                                            OSMemberFunctionCast(IOInterruptEventAction, this, &ApplePS2Controller::interruptOccurred));
	_interruptSourceQueue    = IOInterruptEventSource::interruptEventSource( this,
                                                                            OSMemberFunctionCast(IOInterruptEventAction, this, &ApplePS2Controller::processRequestQueue));
    
    if ( !_workLoop                ||
        !_interruptSourceMouse    ||
        !_interruptSourceKeyboard ||
        !_interruptSourceQueue )  goto fail;
    
    if ( _workLoop->addEventSource(_interruptSourceQueue) != kIOReturnSuccess )
        goto fail;
    
    _interruptSourceQueue->enable();
    
    //
    // Since there is a calling path from the PS/2 driver stack to power
    // management for activity tickles.  We must create a thread callout
    // to handle power state changes from PM to avoid a deadlock.
    //
    
    _powerChangeThreadCall = thread_call_allocate(
                                                  (thread_call_func_t)  setPowerStateCallout,
                                                  (thread_call_param_t) this );
    if ( !_powerChangeThreadCall )
        goto fail;
    
    //
    // Initialize our PM superclass variables and register as the power
    // controlling driver.
    //
    
    PMinit();
    
    registerPowerDriver( this, (IOPMPowerState *) PS2PowerStateArray,
                        kPS2PowerStateCount );
    
    //
    // Insert ourselves into the PM tree.
    //
    
    provider->joinPMtree(this);
    
    //
    // Create the keyboard nub and the mouse nub. The keyboard and mouse drivers
    // will query these nubs to determine the existence of the keyboard or mouse,
    // and should they exist, will attach themselves to the nub as clients.
    //
    
    _keyboardDevice = new ApplePS2KeyboardDevice;
    
    if ( !_keyboardDevice               ||
        !_keyboardDevice->init()       ||
        !_keyboardDevice->attach(this) )  goto fail;
    
    _mouseDevice = new ApplePS2MouseDevice;
    
    if ( !_mouseDevice               ||
        !_mouseDevice->init()       ||
        !_mouseDevice->attach(this) )  goto fail;
    
    gApplePS2Controller = this;
    
    _keyboardDevice->registerService();
    _mouseDevice->registerService();
    return true; // success
    
fail:
    stop(provider);
    return false;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2Controller::stop(IOService * provider)
{
#define RELEASE(x) do { if(x) { (x)->release(); (x) = 0; } } while(0)
    
    //
    // The driver has been instructed to stop.  Note that we must break all
    // connections to other service objects now (ie. no registered actions,
    // no pointers and retains to objects, etc), if any.
    //
    
    // Ensure that the interrupt handlers have been uninstalled (ie. no clients).
    assert(_interruptInstalledKeyboard    == false);
    assert(_interruptInstalledMouse       == false);
    assert(_powerControlInstalledKeyboard == false);
    assert(_powerControlInstalledMouse    == false);
    //PS2 Notifications
    assert(_ps2notificationInstalledKeyboard == false);
    assert(_ps2notifiationInstalledMouseTouchpad == false);
    
    // Free the nubs we created.
    RELEASE(_keyboardDevice);
    RELEASE(_mouseDevice);
    
    // Free the work loop.
    RELEASE(_workLoop);
    
    // Free the interrupt sources.
    RELEASE(_interruptSourceKeyboard);
    RELEASE(_interruptSourceMouse);
    RELEASE(_interruptSourceQueue);
    
    // Free the request queue lock and empty out the request queue.
    if (_requestQueueLock)
    {
        _hardwareOffline = true;
        processRequestQueue(0, 0);
        IOSimpleLockFree(_requestQueueLock);
        _requestQueueLock = 0;
    }
    
    // Free the power management thread call.
    if (_powerChangeThreadCall)
    {
        thread_call_free(_powerChangeThreadCall);
        _powerChangeThreadCall = 0;
    }
    
    // Detach from power management plane.
    PMstop();
    
    gApplePS2Controller = 0;
    
    super::stop(provider);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

IOWorkLoop * ApplePS2Controller::getWorkLoop() const
{
    return _workLoop;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2Controller::installInterruptAction(PS2DeviceType      deviceType,
                                                OSObject *         target,
                                                PS2InterruptAction action)
{
    //
    // Install the keyboard or mouse interrupt handler.
    //
    // This method assumes only one possible mouse and only one possible
    // keyboard client (ie. callers), and assumes two distinct interrupt
    // handlers for each, hence needs no protection against races.
    //
    
    // Is it the keyboard or the mouse interrupt handler that was requested?
    // We only install it if it is currently uninstalled.
    
    if (deviceType == kDT_Keyboard && _interruptInstalledKeyboard == false)
    {
        target->retain();
        _interruptTargetKeyboard = target;
        _interruptActionKeyboard = action;
        _workLoop->addEventSource(_interruptSourceKeyboard);
        getProvider()->registerInterrupt(kIRQ_Keyboard,0, interruptHandlerKeyboard);
        getProvider()->enableInterrupt(kIRQ_Keyboard);
        _interruptInstalledKeyboard = true;
    }
    
    else if (deviceType == kDT_Mouse && _interruptInstalledMouse == false)
    {
        target->retain();
        _interruptTargetMouse = target;
        _interruptActionMouse = action;
        _workLoop->addEventSource(_interruptSourceMouse);
        getProvider()->registerInterrupt(kIRQ_Mouse, 0, interruptHandlerMouse);
        getProvider()->enableInterrupt(kIRQ_Mouse);
        _interruptInstalledMouse = true;
    }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2Controller::uninstallInterruptAction(PS2DeviceType deviceType)
{
    //
    // Uninstall the keyboard or mouse interrupt handler.
    //
    // This method assumes only one possible mouse and only one possible
    // keyboard client (ie. callers), and assumes two distinct interrupt
    // handlers for each, hence needs no protection against races.
    //
    
    // Is it the keyboard or the mouse interrupt handler that was requested?
    // We only install it if it is currently uninstalled.
    
    if (deviceType == kDT_Keyboard && _interruptInstalledKeyboard == true)
    {
        getProvider()->disableInterrupt(kIRQ_Keyboard);
        getProvider()->unregisterInterrupt(kIRQ_Keyboard);
        _workLoop->removeEventSource(_interruptSourceKeyboard);
        _interruptInstalledKeyboard = false;
        _interruptActionKeyboard = NULL;
        _interruptTargetKeyboard->release();
        _interruptTargetKeyboard = 0;
    }
    
    else if (deviceType == kDT_Mouse && _interruptInstalledMouse == true)
    {
        getProvider()->disableInterrupt(kIRQ_Mouse);
        getProvider()->unregisterInterrupt(kIRQ_Mouse);
        _workLoop->removeEventSource(_interruptSourceMouse);
        _interruptInstalledMouse = false;
        _interruptActionMouse = NULL;
        _interruptTargetMouse->release();
        _interruptTargetMouse = 0;
    }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

PS2Request * ApplePS2Controller::allocateRequest()
{
    //
    // Allocate a request structure.  Blocks until successful.  Request structure
    // is guaranteed to be zeroed.
    //
    
    PS2Request * request = (PS2Request *) IOMalloc(sizeof(PS2Request));
    bzero(request, sizeof(PS2Request));
    return request;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2Controller::freeRequest(PS2Request * request)
{
    //
    // Deallocate a request structure.
    //
    
    IOFree(request, sizeof(PS2Request));
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

bool ApplePS2Controller::submitRequest(PS2Request * request)
{
    //
    // Submit the request to the controller for processing, asynchronously.
    //
    
    IOSimpleLockLock(_requestQueueLock);
    queue_enter(&_requestQueue, request, PS2Request *, chain);
    IOSimpleLockUnlock(_requestQueueLock);
    
    _interruptSourceQueue->interruptOccurred(0, 0, 0);
    
    return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2Controller::submitRequestAndBlock(PS2Request * request)
{
    //
    // Submit the request to the controller for processing, synchronously.
    //
    
    if (_workLoop->inGate())
    {
        //
        // Special case to allow PS/2 device drivers to issue synchronous
        // requests from their interrupt handlers. Must process the queue
        // first to process any queued async requests.
        //
        
        request->completionTarget = this;
        request->completionAction = submitRequestAndBlockCompletion;
        request->completionParam  = 0;
        
        processRequestQueue(0, 0);
        processRequest(request);
    }
    else
    {
        IOSyncer * completionSyncer = IOSyncer::create();
        
        assert(completionSyncer);
        request->completionTarget = this;
        request->completionAction = submitRequestAndBlockCompletion;
        request->completionParam  = completionSyncer;
        
        submitRequest(request);
        
        completionSyncer->wait();                               // wait 'till done
    }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2Controller::submitRequestAndBlockCompletion(void *, void * param)
{                                                      // PS2CompletionAction
    if (param)
    {
        IOSyncer * completionSyncer = (IOSyncer *) param;
        completionSyncer->signal();
    }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2Controller::interruptOccurred(IOInterruptEventSource *, int)
{                                                      // IOInterruptEventAction
    //
    // Our work loop has informed us of an interrupt, that is, asynchronous
    // data has arrived on our input stream.  Read the data and dispatch it
    // to the appropriate driver.
    //
    // This method should only be called from our single-threaded work loop.
    //
    
    if (_hardwareOffline)
    {
        // Toss any asynchronous data received. The interrupt event source may
        // have been signalled before the PS/2 port was offline.
        return;
    }
    
    UInt8 status;
    // Loop only while there is data currently on the input stream.
    
    while ( ((status = inb(kCommandPort)) & kOutputReady) )
    {
        // Read in and dispatch the data, but only if it isn't what is required
        // by the active command.
        
        dispatchDriverInterrupt((status&kMouseData)?kDT_Mouse:kDT_Keyboard,
                                inb(kDataPort));
    }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2Controller::dispatchDriverInterrupt(PS2DeviceType deviceType,
                                                 UInt8         data)
{
    //
    // The supplied data is passed onto the interrupt handler in the appropriate
    // driver, if one is registered, otherwise the data byte is thrown away.
    //
    // This method should only be called from our single-threaded work loop.
    //
    
    if ( deviceType == kDT_Mouse )
    {
        // Dispatch the data to the mouse driver.
        if (_interruptInstalledMouse)
            (*_interruptActionMouse)(_interruptTargetMouse, data);
    }
    else if ( deviceType == kDT_Keyboard )
    {
        // Dispatch the data to the keyboard driver.
        if (_interruptInstalledKeyboard)
            (*_interruptActionKeyboard)(_interruptTargetKeyboard, data);
    }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2Controller::processRequest(PS2Request * request)
{
    //
    // Our work loop has informed us of a request submission. Process
    // the request.  Note that this code "figures out" when the mouse
    // input stream should be read over the keyboard input stream.
    //
    // This method should only be called from our single-threaded work loop.
    //
    
    UInt8         byte;
    PS2DeviceType deviceMode      = kDT_Keyboard;
    bool          failed          = false;
    bool          transmitToMouse = false;
    unsigned      index;
    
    if (_hardwareOffline)
    {
        failed = true;
        index  = 0;
        goto hardware_offline;
    }
    
    // Process each of the commands in the list.
    
    for (index = 0; index < request->commandsCount; index++)
    {
        switch (request->commands[index].command)
        {
            case kPS2C_ReadDataPort:
                request->commands[index].inOrOut = readDataPort(deviceMode);
                break;
                
            case kPS2C_ReadDataPortAndCompare:
#if OUT_OF_ORDER_DATA_CORRECTION_FEATURE
                byte = readDataPort(deviceMode, request->commands[index].inOrOut);
#else
                byte = readDataPort(deviceMode);
#endif
                failed = (byte != request->commands[index].inOrOut);
                break;
                
            case kPS2C_WriteDataPort:
                writeDataPort(request->commands[index].inOrOut);
                if (transmitToMouse)     // next reads from mouse input stream
                {
                    deviceMode      = kDT_Mouse;
                    transmitToMouse = false;
                }
                else
                {
                    deviceMode   = kDT_Keyboard;
                }
                break;
                
            case kPS2C_WriteCommandPort:
                writeCommandPort(request->commands[index].inOrOut);
                if (request->commands[index].inOrOut == kCP_TransmitToMouse)
                    transmitToMouse = true; // preparing to transmit data to mouse
                break;
                
                //
                // Send a composite mouse command that is equivalent to the following
                // (frequently used) command sequence:
                //
                // 1. kPS2C_WriteCommandPort( kCP_TransmitToMouse )
                // 2. kPS2C_WriteDataPort( command )
                // 3. kPS2C_ReadDataPortAndCompare( kSC_Acknowledge )
                //
                
            case kPS2C_SendMouseCommandAndCompareAck:
                writeCommandPort(kCP_TransmitToMouse);
                writeDataPort(request->commands[index].inOrOut);
                deviceMode = kDT_Mouse;
#if OUT_OF_ORDER_DATA_CORRECTION_FEATURE
                byte = readDataPort(kDT_Mouse, kSC_Acknowledge);
#else
                byte = readDataPort(kDT_Mouse);
#endif
                failed = (byte != kSC_Acknowledge);
                break;
        }
        
        if (failed) break;
    }
    
hardware_offline:
    
    // If a command failed and stopped the request processing, store its
    // index into the commandsCount field.
    
    if (failed) request->commandsCount = index;
    
    // Invoke the completion routine, if one was supplied.
    
    if (request->completionTarget && request->completionAction)
    {
        (*request->completionAction)(request->completionTarget,
                                     request->completionParam);
    }
    else
    {
        freeRequest(request);
    }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2Controller::processRequestQueue(IOInterruptEventSource *, int)
{
    queue_head_t localQueue;
    
    // Transfer queued (async) requests to a local queue.
    
    IOSimpleLockLock(_requestQueueLock);
    
    if (!queue_empty(&_requestQueue))
    {
        queue_assign(&localQueue, &_requestQueue, PS2Request *, chain);
        queue_init(&_requestQueue);
    }
    else queue_init(&localQueue);
    
    IOSimpleLockUnlock(_requestQueueLock);
    
    // Process each request in order.
    
    while (!queue_empty(&localQueue))
    {
        PS2Request * request;
        queue_remove_first(&localQueue, request, PS2Request *, chain);
        processRequest(request);
    }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

UInt8 ApplePS2Controller::readDataPort(PS2DeviceType deviceType)
{
    //
    // Blocks until keyboard or mouse data is available from the controller
    // and returns that data. Note, if mouse data is requested but keyboard
    // data is what is available,  the data is delivered to the appropriate
    // driver interrupt routine immediately (effectively, the request is
    // "preempted" temporarily).
    //
    // There is a built-in timeout for this command of (timeoutCounter X
    // kDataDelay) microseconds, approximately.
    //
    // This method should only be called from our single-threaded work loop.
    //
    
    UInt8  readByte;
    UInt8  status;
    UInt32 timeoutCounter = 10000;    // (timeoutCounter * kDataDelay = 70 ms)
    
    while (1)
    {
        //
        // Wait for the controller's output buffer to become ready.
        //
        
        while (timeoutCounter && !((status = inb(kCommandPort)) & kOutputReady))
        {
            timeoutCounter--;
            IODelay(kDataDelay);
        }
        
        //
        // If we timed out, something went awfully wrong; return a fake value.
        //
        
        if (timeoutCounter == 0)
        {
            IOLog("%s: Timed out on %s input stream.\n", getName(),
                  (deviceType == kDT_Keyboard) ? "keyboard" : "mouse");
            return 0;
        }
        
        //
        // For older machines, it is necessary to wait a while after the controller
        // has asserted the output buffer bit before reading the data port. No more
        // data will be available if this wait is not performed.
        //
        
        IODelay(kDataDelay);
        
        //
        // Read in the data.  We return the data, however, only if it arrived on
        // the requested input stream.
        //
        
        readByte = inb(kDataPort);
        
        
        if ( (status & kMouseData) )
        {
            if (deviceType == kDT_Mouse)  return readByte;
        }
        else
        {
            if (deviceType == kDT_Keyboard)  return readByte;
        }
        
        //
        // The data we just received is for the other input stream, not the one
        // that was requested, so dispatch other device's interrupt handler.
        //
        
        dispatchDriverInterrupt((deviceType==kDT_Keyboard)?kDT_Mouse:kDT_Keyboard,
                                readByte);
    } // while (forever)
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

#if OUT_OF_ORDER_DATA_CORRECTION_FEATURE

UInt8 ApplePS2Controller::readDataPort(PS2DeviceType deviceType,
                                       UInt8         expectedByte)
{
    //
    // Blocks until keyboard or mouse data is available from the controller
    // and returns that data. Note, if mouse data is requested but keyboard
    // data is what is available,  the data is delivered to the appropriate
    // driver interrupt routine immediately (effectively, the request is
    // "preempted" temporarily).
    //
    // There is a built-in timeout for this command of (timeoutCounter X
    // kDataDelay) microseconds, approximately.
    //
    // This method should only be called from our single-threaded work loop.
    //
    // This version of readDataPort does exactly the same as the original,
    // except that if the value that should be read from the (appropriate)
    // input stream is not what is expected, we make these assumptions:
    //
    // (a) the data byte we did get was  "asynchronous" data being sent by
    //     the device, which has not figured out that it has to respond to
    //     the command we just sent to it.
    // (b) that the real  "expected" response will be the next byte in the
    //     stream;   so what we do is put aside the first byte we read and
    //     wait for the next byte; if it's the expected value, we dispatch
    //     the first byte we read to the driver's interrupt handler,  then
    //     return the expected byte. The caller will have never known that
    //     asynchronous data arrived at a very bad time.
    // (c) that the real "expected" response will arrive within (kDataDelay
    //     X timeoutCounter) microseconds from the time the call is made.
    //
    
    UInt8  firstByte     = 0;
    bool   firstByteHeld = false;
    UInt8  readByte;
    bool   requestedStream;
    UInt8  status;
    UInt32 timeoutCounter = 10000;    // (timeoutCounter * kDataDelay = 70 ms)
    
    while (1)
    {
        //
        // Wait for the controller's output buffer to become ready.
        //
        
        while (timeoutCounter && !((status = inb(kCommandPort)) & kOutputReady))
        {
            timeoutCounter--;
            IODelay(kDataDelay);
        }
        
        //
        // If we timed out, we return the first byte we read, unless THIS IS the
        // first byte we are trying to read,  then something went awfully wrong
        // and we return a fake value rather than lock up the controller longer.
        //
        
        if (timeoutCounter == 0)
        {
            if (firstByteHeld)  return firstByte;
            
            IOLog("%s: Timed out on %s input stream.\n", getName(),
                  (deviceType == kDT_Keyboard) ? "keyboard" : "mouse");
            return 0;
        }
        
        //
        // For older machines, it is necessary to wait a while after the controller
        // has asserted the output buffer bit before reading the data port. No more
        // data will be available if this wait is not performed.
        //
        
        IODelay(kDataDelay);
        
        //
        // Read in the data.  We process the data, however, only if it arrived on
        // the requested input stream.
        //
        
        readByte        = inb(kDataPort);
        requestedStream = false;
        
        if ( (status & kMouseData) )
        {
            if (deviceType == kDT_Mouse)  requestedStream = true;
        }
        else
        {
            if (deviceType == kDT_Keyboard)  requestedStream = true;
        }
        
        if (requestedStream)
        {
            if (readByte == expectedByte)
            {
                if (firstByteHeld == false)
                {
                    //
                    // Normal case.  Return first byte received.
                    //
                    
                    return readByte;
                }
                else
                {
                    //
                    // Our assumption was correct.  The second byte matched.  Dispatch
                    // the first byte to the interrupt handler, and return the second.
                    //
                    
                    dispatchDriverInterrupt(deviceType, firstByte);
                    return readByte;
                }
            }
            else // (readByte does not match expectedByte)
            {
                if (firstByteHeld == false)
                {
                    //
                    // The first byte was received, and does not match the byte we are
                    // expecting.  Put it aside for the moment.
                    //
                    
                    firstByteHeld = true;
                    firstByte     = readByte;
                }
                else if (readByte != expectedByte)
                {
                    //
                    // The second byte mismatched as well.  I have yet to see this case
                    // occur [Dan], however I do think it's plausible.  No error logged.
                    //
                    
                    dispatchDriverInterrupt(deviceType, readByte);
                    return firstByte;
                }
            }
        }
        else
        {
            //
            // The data we just received is for the other input stream, not ours,
            // so dispatch appropriate interrupt handler.
            //
            
            dispatchDriverInterrupt((deviceType==kDT_Keyboard)?kDT_Mouse:kDT_Keyboard,
                                    readByte);
        }
    } // while (forever)
}

#endif

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2Controller::writeDataPort(UInt8 byte)
{
    //
    // Block until room in the controller's input buffer is available, then
    // write the given byte to the Data Port.
    //
    // This method should only be dispatched from our single-threaded work loop.
    //
    
    while (inb(kCommandPort) & kInputBusy)  IODelay(kDataDelay);
    outb(kDataPort, byte);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2Controller::writeCommandPort(UInt8 byte)
{
    //
    // Block until room in the controller's input buffer is available, then
    // write the given byte to the Command Port.
    //
    // This method should only be dispatched from our single-threaded work loop.
    //
    
    while (inb(kCommandPort) & kInputBusy)  IODelay(kDataDelay);
    outb(kCommandPort, byte);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
//
// Power Management support.
//
// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

IOReturn ApplePS2Controller::setPowerState( unsigned long powerStateOrdinal,
                                           IOService *   policyMaker)
{
    IOReturn result = IOPMAckImplied;
    
    //
    // Prevent the object from being freed while a call is pending.
    // If thread_call_enter() returns TRUE, indicating that a call
    // is already pending, then the extra retain is dropped.
    //
    
    retain();
    if ( thread_call_enter1( _powerChangeThreadCall,
                            (void *) powerStateOrdinal ) == TRUE )
    {
        release();
    }
    result = 5000000;  // 5 seconds before acknowledgement timeout
    
    return result;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2Controller::setPowerStateCallout( thread_call_param_t param0,
                                              thread_call_param_t param1 )
{
    ApplePS2Controller * me = (ApplePS2Controller *) param0;
    
    if ( me && me->_workLoop )
    {
        me->_workLoop->runAction( /* Action */ setPowerStateAction,
                                 /* target */ me,
                                 /*   arg0 */ param1 );
    }
    
    me->release();  // drop the retain from setPowerState()
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

IOReturn ApplePS2Controller::setPowerStateAction( OSObject * target,
                                                 void * arg0, void * arg1,
                                                 void * arg2, void * arg3 )
{
    ApplePS2Controller * me = (ApplePS2Controller *) target;
#ifdef __LP64__
	UInt32       powerState = (UInt32)(UInt64)arg0;
#else
    UInt32       powerState = (UInt32) arg0;
#endif
    
    me->setPowerStateGated( powerState );
    
    return kIOReturnSuccess;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2Controller::setPowerStateGated( UInt32 powerState )
{
    UInt8 commandByte;
    
    if ( _currentPowerState != powerState )
    {
        switch ( powerState )
        {
            case kPS2PowerStateSleep:
                
                //
                // Transition from Working state to Sleep state in 3 stages.
                //
                
                // 1. Notify clients about the state change. Clients can issue
                //    synchronous requests thanks to the recursive lock.
                
                dispatchDriverPowerControl( kPS2C_DisableDevice );
                
                // 2. Freeze the request queue and drop all data received over
                //    the PS/2 port.
                
                _hardwareOffline = true;
                
                // 3. Disable the PS/2 port.
                
#if 0
                // This will cause some machines to turn on the LCD after the
                // ACPI display driver has turned it off. With a real display
                // driver present, this block of code can be uncommented (?).
                
                writeCommandPort( kCP_GetCommandByte );
                commandByte = readDataPort( kDT_Keyboard );
                commandByte |=  ( kCB_DisableKeyboardClock |
                                 kCB_DisableMouseClock );
                commandByte &= ~( kCB_EnableKeyboardIRQ |
                                 kCB_EnableMouseIRQ );
                writeCommandPort( kCP_SetCommandByte );
                writeDataPort( commandByte );
#endif
                break;
                
            case kPS2PowerStateDoze:
            case kPS2PowerStateNormal:
                
                if ( _currentPowerState != kPS2PowerStateSleep )
                {
                    // Transitions between doze and normal power states
                    // require no action, since both are working states.
                    break;
                }
                
                //
                // Transition from Sleep state to Working state in 3 stages.
                //
                
                // 1. Enable the PS/2 port.
                
                writeCommandPort( kCP_GetCommandByte );
                commandByte = readDataPort( kDT_Keyboard );
                commandByte &= ~( kCB_DisableKeyboardClock |
                                 kCB_DisableMouseClock );
                commandByte |=  ( kCB_EnableKeyboardIRQ |
                                 kCB_EnableMouseIRQ );
                writeCommandPort( kCP_SetCommandByte );
                writeDataPort( commandByte );
                
                // 2. Unblock the request queue and wake up all driver threads
                //    that were blocked by submitRequest().
                
                _hardwareOffline = false;
                
                // 3. Notify clients about the state change.
                
                dispatchDriverPowerControl( kPS2C_EnableDevice );
                break;
                
            default:
                IOLog("%s: bad power state %d\n", getName(), powerState);
                break;
        }
        
        _currentPowerState = powerState;
    }
    
    //
    // Acknowledge the power change before the power management timeout
    // expires.
    //
    
    acknowledgeSetPowerState();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2Controller::dispatchDriverPowerControl( UInt32 whatToDo )
{
    if (_powerControlInstalledMouse)
        (*_powerControlActionMouse)(_powerControlTargetMouse, whatToDo);
    
    if (_powerControlInstalledKeyboard)       
        (*_powerControlActionKeyboard)(_powerControlTargetKeyboard, whatToDo);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2Controller::installPowerControlAction(
                                                   PS2DeviceType         deviceType,
                                                   OSObject *            target, 
                                                   PS2PowerControlAction action )
{
    if ( deviceType == kDT_Keyboard && _powerControlInstalledKeyboard == false )
    {
        target->retain();
        _powerControlTargetKeyboard = target;
        _powerControlActionKeyboard = action;
        _powerControlInstalledKeyboard = true;
    }
    else if ( deviceType == kDT_Mouse && _powerControlInstalledMouse == false )
    {
        target->retain();
        _powerControlTargetMouse = target;
        _powerControlActionMouse = action;
        _powerControlInstalledMouse = true;
    }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2Controller::uninstallPowerControlAction( PS2DeviceType deviceType )
{
    if ( deviceType == kDT_Keyboard && _powerControlInstalledKeyboard == true )
    {
        _powerControlInstalledKeyboard = false;
        _powerControlActionKeyboard = NULL;
        _powerControlTargetKeyboard->release();
        _powerControlTargetKeyboard = 0;
    }
    else if ( deviceType == kDT_Mouse && _powerControlInstalledMouse == true )
    {
        _powerControlInstalledMouse = false;
        _powerControlActionMouse = NULL;
        _powerControlTargetMouse->release();
        _powerControlTargetMouse = 0;
    }
}

////PS2 Notifications
// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2Controller::installPS2Notification(PS2DeviceType deviceType, OSObject *target, PS2NotificationAction action)
{
    if( deviceType == kDT_Keyboard && _ps2notificationInstalledKeyboard == false)
    {
        target->retain();
        _ps2notificationTargetKeyboard = target;
        _ps2notificationActionKeyboard = action;
        _ps2notificationInstalledKeyboard = true;
    }
    else if( deviceType == kDT_Mouse && _ps2notifiationInstalledMouseTouchpad == false)
    {
        target->retain();
        _ps2notificationTargetMouseTouchpad = target;
        _ps2notificationActionMouseTouchpad = action;
        _ps2notifiationInstalledMouseTouchpad = true;
    }
}

void ApplePS2Controller::uninstallPS2NotificationAction(PS2DeviceType deviceType)
{
    if( deviceType == kDT_Keyboard && _ps2notificationInstalledKeyboard == true)
    {
        _ps2notificationTargetKeyboard->release();
        _ps2notificationTargetKeyboard = 0;
        _ps2notificationActionKeyboard = NULL;
        _ps2notificationInstalledKeyboard = false;
    }
    else if( deviceType == kDT_Mouse && _ps2notifiationInstalledMouseTouchpad == true)
    {
        _ps2notificationTargetMouseTouchpad->release();
        _ps2notificationTargetMouseTouchpad = 0;
        _ps2notificationActionMouseTouchpad = NULL;
        _ps2notifiationInstalledMouseTouchpad = false;
    }
    
}

void ApplePS2Controller::dispatchPS2Notification(PS2DeviceType deviceType, UInt32 data)
{
    if( deviceType == kDT_Keyboard)
    {
        if(_ps2notificationInstalledKeyboard)
            (*_ps2notificationActionKeyboard)(_ps2notificationTargetKeyboard, data);
    }
    if( deviceType == kDT_Mouse)
    {
        if(_ps2notifiationInstalledMouseTouchpad)
            (*_ps2notificationActionMouseTouchpad)(_ps2notificationTargetMouseTouchpad, data);
    }
}
