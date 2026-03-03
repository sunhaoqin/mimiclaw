#!/bin/bash
set -e

# 1. 清理旧条目（忽略"未找到"错误）
grep -v "# GitHub520" /etc/hosts > /tmp/hosts.clean 2>/dev/null || true

# 2. 追加新内容
curl -fsSL https://raw.hellogithub.com/hosts >> /tmp/hosts.clean

# 3. 原子写入（绕过 sed -i 限制）
sudo tee /etc/hosts < /tmp/hosts.clean > /dev/null