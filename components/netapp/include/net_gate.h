/**
 * @file    net_gate.h
 * @brief   全局 HTTPS 串行闸门（一次只允许一个 TLS 连接建立）
 *
 * ★ 2026-11-21 实测教训：
 *   板上有 4 个网络任务（weather / HF / AICPB / B站），各自独立起 HTTPS。
 *   TLS 握手每次要 30KB+ 临时内存，两个连接叠加就分配失败：
 *     mbedtls_ssl_handshake returned -0x3000 (ALLOC_FAILED)
 *     esp-tls-mbedtls: read error :-0x7200 (TLS 记录被破坏)
 *   表现为 B 站接口永远连不上（PC 上 curl 却 200），且偶发拖累天气。
 *   ⇒ 所有 fetch 前 net_gate_take()、结束后 net_gate_give()。
 *     单连接内存峰值可控，代价是并发变串行（本来就是低频任务，无感）。
 */
#pragma once

#include <stdbool.h>

void net_gate_init(void);
bool net_gate_take(int timeout_ms);
void net_gate_give(void);
