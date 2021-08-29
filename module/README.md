# Linux hwmon driver for EK Loop Connect

Kernel module is heavily inspired by the corsair-cpro module, while the protocol
information is based on sniffing USB traffic between the device and the official
EK software, while interacting with the software and controller.


Build and install:

```
make -C /lib/modules/`uname -r`/build M=$PWD

make -C /lib/modules/`uname -r`/build M=$PWD modules_install
```
