# 脚化编辑的转义陷阱家族与生成代码调试法（JS 模板 × heredoc × python 三层）

> 2026-10-09 · 本会话 60+ 轮脚本化编辑的集中复盘；事故均为已发生实录，非推断。
> 相关文件：scripts/release_guard.py、scripts/test_release_guard.py、Doc/Plan/backlog.md、
> desktop/macos/Tests/VoiceStickTests/CoordinatorFsmTests.swift、firmware/main/main.c
> 代表 commit：75605c4（8/12 空格子串）· 8a64776（粘行越位）· 6a22995（repr 剥引号）·
> bb4780d（换行粘连）· ceab194（printf 换行）· ea0554e（码点记错）

## 症状 → 判据（下次见这些报错直接对号）

| 报错/现象 | 真实根因 |
|---|---|
| JS 侧 "Expected ',' got 'ident'" / "Unterminated template" | 待写内容里有**裸反引号**（代码围栏三连、行内代码 span）关掉了模板字符串 |
| JS 侧 "Expected ';'..." / python "EOL while scanning string literal" | 裸双引号/单引号冲突（repr[1:-1] 类拼装、嵌套字符串） |
| python SyntaxError 指向字符串中段 | 反斜杠-引号转义被 JS 模板吞掉一层（写入的 \" 变成 "） |
| C 编译 C2001 newline in string literal | 双反斜杠+n（字面 \n）被逐层吞成**真换行**（printf 落成两行） |
| grep \b 行为异常/输出乱 | JS 模板把 \b 变成退格字符 0x08 传给了命令（转义坑第 7 例） |
| commit stat 异常 1 insertion + 1 deletion（只想新增时） | 插入块**缺尾换行**，吞掉了下一行（胶带家族，≥5 例） |
| 脚本 RC=0 却毫无输出、文件没变 | 尾部被截断（如 s[:i] 误切 write/print）= 空转假成功 |
| 校验式自身也断言失败 | 校验式复用了同一个坏转义——**自检也会中同一坑**（r127 printf 案） |

## 根因

三层叠加：① LLM 写 JS template literal（run_code/edit/write 的 content 参数）时，反引号、
反斜杠、美元-花括号插值都要一层转义，而双反斜杠+n 只能存活一层；② bash heredoc（带引号
的 EOF）本身不处理转义，但**模板层已经先处理过**，python 收到被剥后的文本；③ 人写拼装
表达式（repr 去首尾、字符串加字符串）容易把引号当数据剥掉。

## 修复（本会话固化的工作法，按可靠性排序）

1. **chr 圣经**：反引号 chr(96)、双引号 chr(34)、单引号 chr(39)、反斜杠 chr(92)、
   换行 chr(10)——一律变量拼装，不写字面量（占位符再 replace 亦可）。
2. **write 工具先行**：复杂脚本先用 write 落文件（多一道人工审读），再 python3 执行。
3. **双门前置**：ast.parse / bash -n / py_compile 先过再跑；assert old in t 锚失败=零落盘（好刹车）。
4. **粘连防线**：任何插入行/块必须显式以换行收尾；见 1+1 反常 diff 或相邻行首尾相接 → 立即拆行。
5. **实文优先**：报错先 sed -n / read 看真实字节，别对着自己的构造表达式推演。

## 修复实例（机制 + 判据）

- **printf 换行（ceab194）**：字面反斜杠+n 逐层变真换行 → chr(92)+n 重建；随后**校验式自身**
  同坑（又去搜真换行）→ 第二次 chr 化才过。判据：文件肉眼 grep 正常但断言永假。
- **胶带族（bb4780d 等 ≥5 例）**：范式文档插入吞掉「固件」条目、D11 行尾粘住 Run() 首行、
  guard 函数 return 与下语句粘连（RC=0 空转，因切片误伤 write）、fixture fh.write 粘连、
  D11 行内换行破表。判据：1+1 异常 diff、相邻行相接、python RC=0 无输出。
- **生成代码隐伤（r128 教材）**：check_control_frame_schema 的生成段三轮探针（DBG/异常/行级）
  全部"矛盾" → **止损：整段手写重写，一次通过**。经验阈值：生成代码调试两轮无果即人写替代，
  别跟生成器缠斗。

## 验证

本文明列事故均以本会话 commit、CI 结果或文件实文为准（时效性：行号/计数为记录时点，
引用前以当前源码为准）。无未验证断言。

## 遗留/观察

该家族**跨会话复发**（8a64776、6a22995、ea0554e 等多次）→ 已挂 claude-memory-distilled §10
速查一行；未来任何「JS 模板写文件」动作，先过上面的判据表。
