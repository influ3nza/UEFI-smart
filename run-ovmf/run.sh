qemu-system-x86_64 \
	-m 4096 \
	-smp 4 \
	-pflash ../Build/OvmfX64/DEBUG_GCC5/FV/OVMF.fd \
	-machine q35,smm=on \
	-kernel $1/arch/x86/boot/bzImage \
	-append "console=ttyS0 root=/dev/sda nokaslr" \
	-drive file=image/bullseye.img,format=raw \
	-virtfs local,path=hda-contents,mount_tag=host0,security_model=passthrough,id=host0 \
	-nographic \
	-pidfile vm.pid \
	-debugcon file:debug.log \
	-global isa-debugcon.iobase=0x402 \
	2>&1 | tee vm.log
