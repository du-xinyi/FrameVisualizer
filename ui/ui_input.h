#pragma once

struct AppState;

/**
 * @brief 将应用快捷键转换为显示、历史浏览或退出请求
 *
 * @param key ASCII 键值或约定的控制键码
 * @param state 接收快捷键结果的应用状态
 */
void handleKey(int key, AppState &state);
