#!/bin/bash
# test_envoy_proxy.sh

echo "=== Envoy Proxy 验证测试 ==="

# 1. 检查管理接口
echo "1. 检查管理接口:"
curl -s http://localhost:9901/server_info | grep -q "live" && echo "✓ 管理接口正常" || echo "✗ 管理接口异常"

# 2. 检查TCP代理端口
echo "2. 检查TCP代理端口:"
nc -zv localhost 10000 2>&1 | grep -q "succeeded" && echo "✓ TCP代理端口正常" || echo "✗ TCP代理端口异常"

# 3. 检查UDP代理端口
echo "3. 检查UDP代理端口:"
nc -zuv localhost 10001 2>&1 | grep -q "succeeded" && echo "✓ UDP代理端口正常" || echo "✗ UDP代理端口异常"

# 4. 检查FTP端口
echo "4. 检查FTP控制端口:"
nc -zv localhost 21 2>&1 | grep -q "succeeded" && echo "✓ FTP控制端口正常" || echo "✗ FTP控制端口异常"

# 5. 检查H.323端口
echo "5. 检查H.323信令端口:"
nc -zv localhost 1720 2>&1 | grep -q "succeeded" && echo "✓ H.323信令端口正常" || echo "✗ H.323信令端口异常"

# 6. 检查TFTP端口
echo "6. 检查TFTP端口:"
nc -zuv localhost 69 2>&1 | grep -q "succeeded" && echo "✓ TFTP端口正常" || echo "✗ TFTP端口异常"

echo "=== 测试完成 ==="