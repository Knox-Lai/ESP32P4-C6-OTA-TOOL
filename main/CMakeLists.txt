idf_component_register(
    # 源文件按字母序，避免同一文件重复登记（CMake 会静默去重，但列表会越来越乱）
    SRCS "app_main.c"
         "bsp_display.c"
         "bsp_sdcard.c"
         "dev_discovery.c"
         "dev_model.c"
         "dev_table.c"
         "fw_pkg.c"
         "fw_store.c"
         "i18n.c"
         "net_dhcps.c"
         "net_eth.c"
         "net_wifi.c"
         "ota_client.c"
         "ota_frame.c"
         "ota_image.c"
         "ota_proto.c"
         "ota_sha256.c"
         "ota_shell.c"
         "sys_clock.c"
         "ui_app.c"
         # 生成的字库（tools/fontgen/gen_fonts.py 产出）：
         #   PingFang SC Regular 子集（16/22/30 px，中文）
         #   SF Pro Text Regular 的 ASCII 子集（14/20/28 px，英文）
         #   iconfont 子集
         # 改了 i18n.c 里的文案后必须重跑脚本，否则新字会直接显示成方块。
         "fonts/ui_font_zh_16.c"
         "fonts/ui_font_zh_22.c"
         "fonts/ui_font_zh_30.c"
         "fonts/ui_font_en_14.c"
         "fonts/ui_font_en_20.c"
         "fonts/ui_font_en_28.c"
         "fonts/ui_icon_40.c"
         "fonts/ui_icon_24.c"
    INCLUDE_DIRS "."
    # 依赖必须写全，否则链接期才报 undefined reference。
    #   esp_eth        : EMAC 驱动、netif glue、ioctl
    #   esp_netif      : netif 与 dhcps API
    #   esp_event      : 事件循环、IP_EVENT 租约事件
    #   lwip           : dhcps_lease_t、BSD socket（sendto/recvfrom/connect/select）
    #   esp_timer      : 毫秒时间戳（心跳、重试、会话计时）
    #   esp_driver_uart: 命令行（ota_shell）读 UART0
    #   esp_driver_usb_serial_jtag: 命令行同时读板载 USB-JTAG 口（二级控制台）
    #   esp_common     : esp_check.h (ESP_RETURN_ON_ERROR / ESP_RETURN_ON_FALSE)
    #   log / freertos : 日志、任务、互斥量
    #
    # 阶段 8 显示链路：
    #   esp_lcd                    : 面板/IO 通用 API
    #   esp_lcd_jd9165             : 7 寸屏驱动 IC
    #   esp_lcd_touch / gt911      : 电容触摸
    #   esp_lvgl_port / lvgl       : LVGL 9 与 FreeRTOS 端口
    #   esp_driver_ledc            : 背光 PWM
    #   esp_driver_i2c             : 触摸 I2C
    #   esp_hw_support             : esp_ldo_regulator.h（DSI PHY 供电）
    #
    # espressif__ip101 由 main/idf_component.yml 自动注入
    #
    # SHA-256 是自己实现的（ota_sha256.c），不引入 mbedtls：
    # IDF 6.x 是 mbedtls 4.x，`mbedtls/sha256.h` 已删除，只剩 PSA 一套，
    # 为一个整包摘要去初始化 PSA 不值得，也会把组件依赖面扩大。
    # spi_flash：esp_flash_get_size() 读本机 Flash 实际容量（信息页显示）
    # 阶段 9 TF 卡（bsp_sdcard.c）：
    #   esp_driver_sdmmc : SDMMC 主机驱动（SDMMC_HOST_DEFAULT / 槽位配置）
    #   sdmmc            : sd_pwr_ctrl_by_on_chip_ldo.h（片内 LDO 给卡供电）
    #   fatfs            : esp_vfs_fat_sdmmc_mount / esp_vfs_fat_info
    # 阶段 10 固件仓库（fw_store.c）：
    #   wear_levelling   : esp_vfs_fat_spiflash_mount_rw_wl（storage 分区 FAT + WL）
    # WiFi（net_wifi.c）：
    #   esp_wifi         : esp_wifi.h / esp_wifi_default.h。P4 上没有本地射频，
    #                      该组件按 CONFIG_ESP_WIFI_REMOTE_LIBRARY_HOSTED 编译成
    #                      "转发给 C6" 的远程实现，头文件与 API 与本地完全一致。
    REQUIRES esp_eth esp_netif esp_event lwip esp_timer esp_driver_uart
             esp_driver_usb_serial_jtag esp_common log freertos nvs_flash
             esp_lcd esp_lcd_jd9165 esp_lcd_touch esp_lcd_touch_gt911
             esp_lvgl_port lvgl esp_driver_ledc esp_driver_i2c esp_hw_support
             spi_flash esp_driver_sdmmc sdmmc fatfs wear_levelling esp_wifi
)
