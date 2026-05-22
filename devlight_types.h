#pragma once
// 放在独立头文件,避免 Arduino .ino 自动原型注入在枚举定义之前导致编译错误。
enum SubState { ST_IDLE, ST_WORKING, ST_CONFIRM };
enum Agg { AGG_GREEN, AGG_YELLOW, AGG_RED };
