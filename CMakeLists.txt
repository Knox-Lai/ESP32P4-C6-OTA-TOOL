cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 阶段 8 起引入 LVGL：它通过 esp_lvgl_port 间接依赖 esp_lcd / esp_timer / freertos，
# 依赖面太大，MINIMAL_BUILD 已经划不清边界（会漏掉 lvgl 的编译单元），
# 因此这里关掉它，回到 IDF 默认的全量组件构建。
#
# 代价（相对阶段 5）：
#   - 首次全量编译 ~5-8 分钟（之后增量正常）
#   - 镜像 389 KB -> 2.5~3.5 MB，分区 factory 已从 3 MB 扩到 6 MB
#
# idf_build_set_property(MINIMAL_BUILD ON)
project(esp32p4_eth_ota)
