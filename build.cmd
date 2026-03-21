set PATH=C:\ncs\toolchains\66cdf9b75e\mingw64\bin;C:\ncs\toolchains\66cdf9b75e\bin;C:\ncs\toolchains\66cdf9b75e\opt\bin;C:\ncs\toolchains\66cdf9b75e\opt\bin\Scripts;C:\ncs\toolchains\66cdf9b75e\opt\nanopb\generator-bin;C:\ncs\toolchains\66cdf9b75e\nrfutil\bin;C:\ncs\toolchains\66cdf9b75e\opt\zephyr-sdk\arm-zephyr-eabi\bin;C:\ncs\toolchains\66cdf9b75e\opt\zephyr-sdk\riscv64-zephyr-elf\bin;%PATH%
set PYTHONPATH=C:\ncs\toolchains\66cdf9b75e\opt\bin;C:\ncs\toolchains\66cdf9b75e\opt\bin\Lib;C:\ncs\toolchains\66cdf9b75e\opt\bin\Lib\site-packages
set NRFUTIL_HOME=C:\ncs\toolchains\66cdf9b75e\nrfutil\home
set ZEPHYR_TOOLCHAIN_VARIANT=zephyr
set ZEPHYR_SDK_INSTALL_DIR=C:\ncs\toolchains\66cdf9b75e\opt\zephyr-sdk
set ZEPHYR_BASE=C:\ncs\v3.2.1\zephyr
west build -b promicro_nrf52840 --no-sysbuild -p
python uf2conv.py -f 0xada52840 -c -o build/zephyr/zephyr.uf2 build/zephyr/zephyr.hex