# PS2Keyboard

This is aimed squarely at OS X 10.12 Sierra, the brightness keys would need to be altered in the source for El Cap.  
There's no caps lock fix implemented because the solution causes more problems than it's worth, just hit it twice.  

The purpose of this project is a tailored PS2 keyboard solution for the HP Elite X2 1012 G2 (it *may* work for the G1 as well).
I've used VoodooPS2 in the past, but always had issues with lid initiated sleep/wake using it.  I've also used the SmartTouchPad kext
that is primarily meant for the PS2 Elan touchpads and while it definitely works, it is closed source and probably shouldn't 
be.  Not to mention that SmartTouchPad is absent some really great features available in VoodooPS2.  This is my attempt at 
melding some different parts to create something that is keyboard focused (since that is all that is needed) and reliable.

The bulk of the source code is Apple's own PS2 source code.  Snippets were used from Rehabman's VoodooPS2Controller 
located here - https://github.com/RehabMan/OS-X-Voodoo-PS2-Controller . Most notably, the ACPI keyboard
methods for the Elite's hard volume keys, though greatly simplified since this is a targeted solution.  Additionally, 
the accidental keypress routines were taken from here - https://github.com/EMlyDinEsHMG/ElanTouchpad-Driver 

The only changes that I'm likely to make going forward would be to replace deprecated functions -- ie IOSyncer, provided
that reliability can be maintained.


----------------------
ACPI key press methods
----------------------
Adding keypresses from ACPI is simple.  No changes were made to the way the stock ApplePS2Keyboard accepts input so each
Extended scan code requires 4 lines.  For example, to send volume up (e0 30) you end up with:

                    Notify (\_SB.PCI0.LPCB.PS2K, 0xE0)
                    Notify (\_SB.PCI0.LPCB.PS2K, 0x30)
                    Notify (\_SB.PCI0.LPCB.PS2K, 0xE0)
                    Notify (\_SB.PCI0.LPCB.PS2K, 0xB0)
                    
Which is nothing more than the scan codes and break codes sent in sequence.  This can be tailored to anthing that's valid in
the keymap for regular scan codes, but extended codes will need to be modified in the dispatchKeyboardEventWithScancode function
in ApplePS2Keyboard.cpp.


If all you need is a PS2 Keyboard like myself (my touchpad is USB), adapting it for other devices by altering the should be
fairly simple.
