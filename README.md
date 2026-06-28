# LiquidOS
This is an operating system I have been making from scratch.

## Running LiquidOS in Qemu
### You will need:
-The .zip (the one from releases or source code doesn't matter)  
-Make command line tool
### Windows
¯\_(ツ)_/¯
### Linux
You should have make already installed if you do unzip the zip file cd into the folder and run "make run"  
If you dont have make and your on a debian based distro run "sudo apt install build-essential" otherwise ¯\_(ツ)_/¯
### macOS
Same instructions as linux but if you have not downloaded make already you need to run "xcode-select --install"

## Running LiquidOS on real hardware
### You will need:
-Something to store the image like a usb flash drive (any size the image is less than a gigabyte right now)
-A computer (The official testing computer is a HP ProBook 6450b)
### Installation
Do the same steps for running LiquidOS in Qemu for your operating system then do these steps:  
1. after you run LiquidOS in Qemu go to the folder containing the makefile then cd into the build folder
2. Flash the .iso image in the folder onto your usb flash drive
3. Once it is flashed boot off the flash drive on your computer
4. If you want to install LiquidOS to the internal storage drive follow these steps again but flash to the drive as (currently) there is no way to install LiquidOS to your internal storage drive without flashing to it directly.
