#!/usr/bin/env python3
"""
GPON/EPON 驱动代码验证器

没有 C 编译器的情况下，用三层验证尽可能接近真实编译：

  1. 语法层：pycparser 做真实的 C99 语法解析（能抓括号/声明/语句错误）
  2. 符号层：检查所有函数调用是否都有定义或声明（抓未定义符号）
  3. 内核 API 层：检查用到的内核函数/宏是否真实存在（抓 API 误用）

用法：
  python tools/verify_driver.py src/airoha-xpon-gpon.c
  python tools/verify_driver.py src/*.c --headers src/*.h
"""
import argparse
import glob
import os
import re
import sys

# 内核头文件桩：把用到的内核 API 声明出来，让 pycparser 能解析
KERNEL_STUB = r"""
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef signed char s8;
typedef signed short s16;
typedef signed int s32;
typedef signed long long s64;
typedef unsigned long size_t;
typedef long ssize_t;
typedef int bool;
enum { false_v = 0, true_v = 1 };
static const int true = 1;
static const int false = 0;
static void *const NULL = 0;

static unsigned long BIT(int n) { return 1UL << n; }
static unsigned long GENMASK(int h, int l) { return 1; }
static unsigned long ARRAY_SIZE(int a) { return 1; }
static unsigned long FIELD_PREP(unsigned long m, unsigned long v) { return v; }
static unsigned long READ_ONCE(unsigned long x) { return x; }
static void WRITE_ONCE(void *p, unsigned long v) { }
static void EXPORT_SYMBOL_GPL(int x) { }
static void MODULE_LICENSE(const char *x) { }
static void MODULE_DESCRIPTION(const char *x) { }
static void MODULE_AUTHOR(const char *x) { }

struct device;
struct seq_file;
struct sk_buff;
struct timer_list;
struct tasklet_struct;
struct work_struct;
struct net_device;
struct dentry;
struct mutex;
struct module;

struct list_head { struct list_head *next, *prev; };
struct timer_list {
    unsigned long expires;
    void (*function)(struct timer_list *);
    unsigned long data;
};
struct tasklet_struct {
    void (*func)(unsigned long);
    unsigned long data;
};

unsigned int readl(const volatile void *addr);
void writel(unsigned int value, volatile void *addr);
void udelay(unsigned long usecs);
void mdelay(unsigned long msecs);
void msleep(unsigned int msecs);
unsigned long jiffies;
int mod_timer(struct timer_list *timer, unsigned long expires);
int del_timer(struct timer_list *timer);
int del_timer_sync(struct timer_list *timer);
void tasklet_schedule(struct tasklet_struct *t);
void tasklet_init(struct tasklet_struct *t, void (*func)(unsigned long), unsigned long data);

int dev_err(const struct device *dev, const char *fmt, ...);
int dev_warn(const struct device *dev, const char *fmt, ...);
int dev_info(const struct device *dev, const char *fmt, ...);
int dev_dbg(const struct device *dev, const char *fmt, ...);
int printk(const char *fmt, ...);
int seq_printf(struct seq_file *m, const char *fmt, ...);

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);

typedef struct { int counter; } atomic_t;
int atomic_read(const atomic_t *v);
void atomic_set(atomic_t *v, int i);
int atomic_xchg(atomic_t *v, int i);

struct mutex { int dummy; };
void mutex_lock(struct mutex *lock);
void mutex_unlock(struct mutex *lock);

typedef unsigned long irqreturn_t;
static const int IRQ_NONE = 0;
static const int IRQ_HANDLED = 1;
static const int IRQ_WAKE_THREAD = 2;

typedef unsigned long ktime_t;

static const int EPERM = 1;
static const int ENOENT = 2;
static const int EIO = 5;
static const int ENXIO = 6;
static const int EAGAIN = 11;
static const int ENOMEM = 12;
static const int EFAULT = 14;
static const int EBUSY = 16;
static const int EEXIST = 17;
static const int ENODEV = 19;
static const int EINVAL = 22;
static const int ENOSPC = 28;
static const int ENOSYS = 38;
static const int ETIMEDOUT = 110;
static const int EOPNOTSUPP = 95;

unsigned long msecs_to_jiffies(const unsigned int m);
int time_after(unsigned long a, unsigned long b);
int time_before(unsigned long a, unsigned long b);
"""


def strip_comments_c(src):
    """状态机剥离 C 注释（正确处理字符串字面量里的 // 与 /*）"""
    out = []
    i, n = 0, len(src)
    state = "code"      # code | line | block | str | chr
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if state == "code":
            if c == "/" and nxt == "/":
                state = "line"; i += 2; continue
            if c == "/" and nxt == "*":
                state = "block"; i += 2; continue
            if c == '"':
                state = "str"; out.append(c); i += 1; continue
            if c == "'":
                state = "chr"; out.append(c); i += 1; continue
            out.append(c); i += 1
        elif state == "line":
            if c == "\n":
                state = "code"; out.append(c)
            i += 1
        elif state == "block":
            if c == "*" and nxt == "/":
                state = "code"; i += 2
                out.append(" ")     # 保留一个空格避免粘连
                continue
            i += 1
        elif state == "str":
            out.append(c)
            if c == "\\":
                if i + 1 < n:
                    out.append(src[i + 1]); i += 2; continue
            if c == '"':
                state = "code"
            i += 1
        elif state == "chr":
            out.append(c)
            if c == "\\":
                if i + 1 < n:
                    out.append(src[i + 1]); i += 2; continue
            if c == "'":
                state = "code"
            i += 1
    return "".join(out)


def strip_preprocessor(src):
    """极简预处理：展开本文件的 #define，去掉 #include/#ifdef 等指令

    pycparser 不支持预处理指令，所以这里做一个最小实现：
      - #define NAME value  → 记录并替换（只处理对象式宏）
      - #define NAME(args) → 记录名字，调用处替换为 NAME
      - #include / #if / #ifdef / #endif / #pragma → 删除
    """
    # 合并续行符（反斜杠 + 换行），否则多行宏会被拆断
    src = re.sub(r"\\\r?\n", " ", src)

    defines = {}
    lines = src.split("\n")
    out = []
    for line in lines:
        st = line.lstrip()
        if st.startswith("#include") or st.startswith("#pragma"):
            continue
        if st.startswith("#ifdef") or st.startswith("#ifndef") or \
           st.startswith("#if ") or st.startswith("#if(") or \
           st.startswith("#else") or st.startswith("#endif") or \
           st.startswith("#elif") or st.startswith("#undef"):
            continue
        m = re.match(r"\s*#define\s+(\w+)(\([^)]*\))?\s*(.*)$", line)
        if m:
            name, params, val = m.group(1), m.group(2), m.group(3).strip()
            if params:
                # 函数式宏：记录参数个数，调用处做简单展开太复杂，先只记录名字
                defines[name] = ("fn", params)
            else:
                defines[name] = ("obj", val)
            out.append("")
            continue
        out.append(line)

    body = "\n".join(out)

    # 对象式宏替换（迭代 3 轮，处理嵌套）
    for _ in range(3):
        for name, (kind, val) in defines.items():
            if kind != "obj" or not val:
                continue
            # 整词替换
            body = re.sub(r"\b" + re.escape(name) + r"\b", "(" + val + ")", body)

    # typedef 名保护：typedef int (*xxx_fn)(...) 里的 fn 不是宏
    typedef_names = set(re.findall(r'typedef\s+[^;]*?\b(\w+)\s*\)\s*\(', body))
    for tn in typedef_names:
        defines.pop(tn, None)

    # 函数式宏：把整个 NAME(...) 替换成一条空语句，避免语法错误
    # 注意：要处理嵌套括号，这里用简单的括号计数
    for name, (kind, _p) in defines.items():
        if kind != "fn":
            continue
        out = []
        i = 0
        n = len(body)
        pat = re.compile(r"\b" + re.escape(name) + r"\s*\(")
        while i < n:
            m = pat.search(body, i)
            if not m:
                out.append(body[i:])
                break
            out.append(body[i:m.start()])
            # 从 '(' 开始做括号计数
            j = m.end() - 1
            depth = 0
            while j < n:
                if body[j] == "(":
                    depth += 1
                elif body[j] == ")":
                    depth -= 1
                    if depth == 0:
                        j += 1
                        break
                j += 1
            # 替换为分号（保留语句结构）
            out.append(";")
            i = j
        body = "".join(out)

    # 最后清掉 EXPORT_SYMBOL 之类的内核声明宏（它们在 stub 里是函数，
    # 但在源文件里是宏调用形式）
    for kw in ("EXPORT_SYMBOL_GPL", "EXPORT_SYMBOL", "MODULE_LICENSE",
               "MODULE_DESCRIPTION", "MODULE_AUTHOR", "MODULE_VERSION",
               "MODULE_ALIAS", "MODULE_DEVICE_TABLE"):
        body = re.sub(r"\b" + kw + r"\s*\([^;{}]*?\)\s*;", "", body)

    return body


def strip_kernel_macros(src):
    """把 pycparser 处理不了的内核特有写法做最小替换"""
    # 最后再剥一次注释（宏展开可能把注释带进括号里）
    src = strip_comments_c(src)
    src = re.sub(r'__attribute__\s*\(\([^)]*\)\)', '', src)
    src = re.sub(r'\basm\s+volatile\s*\([^;]*\);', '', src)
    # 续行符
    src = src.replace('\\\n', '\n')
    return src


def check_syntax(path):
    """用 pycparser 做真实语法解析"""
    from pycparser import c_parser, c_generator
    src = open(path, encoding='utf-8').read()
    body = strip_preprocessor(src)
    body = strip_kernel_macros(body)
    full = KERNEL_STUB + "\n" + body

    parser = c_parser.CParser()
    try:
        parser.parse(full, filename=path)
        return True, None
    except Exception as e:
        return False, str(e)


def collect_decls(src):
    """收集本文件里的函数定义与声明（支持多行参数列表）"""
    # 函数定义：以 { 结尾，参数列表允许跨行
    defs = set()
    for m in re.finditer(
            r'^(?:static\s+)?(?:inline\s+)?(?:const\s+)?'
            r'[\w\s\*]+?\b(\w+)\s*\([^;{}]*?\)\s*\n?\{',
            src, re.M | re.S):
        defs.add(m.group(1))
    # 函数声明：以 ; 结尾
    decls = set()
    for m in re.finditer(
            r'^(?:extern\s+)?(?:static\s+)?(?:inline\s+)?(?:const\s+)?'
            r'[\w\s\*]+?\b(\w+)\s*\([^;{}]*?\)\s*;',
            src, re.M | re.S):
        decls.add(m.group(1))
    return defs, decls


def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)
    return src


def check_symbols(path, extra_headers=()):
    """检查函数调用是否都有定义或声明"""
    src = strip_comments(open(path, encoding='utf-8').read())
    # 去掉字符串字面量，避免 "for us" 里的 us 被当成函数
    src = re.sub(r'"(?:[^"\\]|\\.)*"', '""', src)
    defs, decls = collect_decls(src)

    # 头文件里声明的也算
    hdr_decls = set()
    for h in extra_headers:
        try:
            hsrc = open(h, encoding='utf-8').read()
            _, hd = collect_decls(hsrc)
            hdr_decls |= hd
            # 头文件里的 extern 声明
            for m in re.finditer(r'\b(\w+)\s*\([^;{]*?\)\s*;', hsrc):
                hdr_decls.add(m.group(1))
        except OSError:
            pass

    # 找出所有形如 foo( 的调用
    calls = set()
    for m in re.finditer(r'\b([a-z_][a-z0-9_]*)\s*\(', src):
        calls.add(m.group(1))

    # 过滤掉关键字与内核 API（stub 已声明）
    keywords = {
        'if', 'while', 'for', 'switch', 'return', 'sizeof', 'typeof',
        'case', 'do', 'else', 'defined', 'min', 'max',
    }
    known_kernel = set(re.findall(r'\b(\w+)\s*\(', KERNEL_STUB))

    # 已知的上游/内核符号（由外部模块或内核提供）
    external = {
        # 上游 airoha-xpon 提供的
        'airoha_xpon_get_hir', 'airoha_xpon_omci_receive',
        'airoha_xpon_omci_wake_tx', 'airoha_xpon_oam_receive',
        'airoha_xpon_oam_wake_tx', 'airoha_xpon_notify_event',
        'airoha_xpon_leds_update', 'airoha_xpon_enter_popup',
        # 内核提供的
        'swab32', 'swab16', 'swahw32', 'cpu_to_be32', 'be32_to_cpu',
        'readl_poll_timeout_atomic', 'udelay', 'ndelay',
        'tasklet_schedule', 'tasklet_init', 'tasklet_kill',
        'schedule_work', 'schedule_delayed_work', 'cancel_work_sync',
        'cancel_delayed_work_sync', 'flush_work', 'flush_delayed_work',
        'devm_kzalloc', 'devm_kfree', 'kzalloc', 'kfree',
        'platform_get_irq', 'devm_request_irq', 'request_irq', 'free_irq',
        'of_property_read_u32', 'of_get_child_by_name',
        'regmap_read', 'regmap_write', 'regmap_update_bits',
        'netif_carrier_on', 'netif_carrier_off', 'netif_rx',
        'ether_addr_copy', 'is_valid_ether_addr',
        'seq_puts', 'seq_putc', 'single_open', 'seq_read',
    }
    unknown = calls - defs - decls - hdr_decls - keywords - known_kernel - external
    unknown = {u for u in unknown if not u.isupper()}
    return sorted(unknown)


def check_kernel_api(path):
    """检查是否用了未在 stub 中声明的内核 API（可能是误用）"""
    src = strip_comments(open(path, encoding='utf-8').read())
    known = set(re.findall(r'\b(\w+)\s*\(', KERNEL_STUB))
    # 常见内核前缀
    suspicious = []
    for m in re.finditer(r'\b((?:read|write|dev_|kmalloc|kfree|copy_|'
                         r'netif_|napi_|skb_|timer_|tasklet_|work_|'
                         r'schedule_|cancel_|flush_|spin_|mutex_|'
                         r'platform_|devm_|of_|clk_|regmap_)\w*)\s*\(', src):
        fn = m.group(1)
        if fn not in known:
            suspicious.append(fn)
    return sorted(set(suspicious))


def check_macro_consistency(c_files, h_files):
    """检查宏使用与定义的一致性（只查 AIROHA_ 前缀，避免误报）"""
    all_defs = set()
    for h in h_files:
        try:
            s = open(h, encoding='utf-8').read()
            all_defs |= set(re.findall(r'#define\s+(\w+)', s))
        except OSError:
            pass

    results = {}
    for c in c_files:
        s = strip_comments(open(c, encoding='utf-8').read())
        local_defs = set(re.findall(r'#define\s+(\w+)', s))
        enums = set(re.findall(r'\b(\w+)\s*=\s*', s))
        # 只查项目自有宏（AIROHA_ 前缀），内核宏与错误码由编译器保证
        used = set(re.findall(r'\b(AIROHA_[A-Z0-9_]+)\b', s))
        missing = used - all_defs - local_defs - enums
        results[c] = sorted(missing)
    return results


def check_balance(path):
    """括号配平 + 常见笔误"""
    src = open(path, encoding='utf-8').read()
    issues = []
    if src.count('{') != src.count('}'):
        issues.append(f"花括号不配平: {src.count('{')} vs {src.count('}')}")
    if src.count('(') != src.count(')'):
        issues.append(f"圆括号不配平: {src.count('(')} vs {src.count(')')}")

    # 检查 EXPORT_SYMBOL 后面是否有对应定义
    # 用「函数名 + (」直接搜，比正则解析声明更可靠
    exported = re.findall(r'EXPORT_SYMBOL_GPL\((\w+)\)', src)
    for e in exported:
        # 函数定义：行首类型 + 名字(
        as_func = re.search(
            r'^(?:static\s+)?(?:inline\s+)?(?:const\s+)?'
            r'[\w\s\*]+?\b' + re.escape(e) + r'\s*\([^;{}]*?\)\s*\n?\{',
            src, re.M | re.S)
        # 全局变量定义：... 名字 = 或 名字[ = 
        as_var = re.search(
            r'^(?:const\s+)?(?:struct|union|enum)?\s*[\w\s\*]+?\b' +
            re.escape(e) + r'\s*(?:\[[^\]]*\])?\s*=',
            src, re.M)
        if not as_func and not as_var:
            issues.append(f"EXPORT_SYMBOL_GPL({e}) 但本文件无该定义")
    return issues


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+", help="要验证的 .c 文件")
    ap.add_argument("--headers", nargs="*", default=[], help="配套头文件")
    ap.add_argument("--skip-syntax", action="store_true",
                    help="跳过 pycparser 语法检查（内核代码常因宏失败）")
    args = ap.parse_args()

    c_files = []
    for f in args.files:
        c_files.extend(glob.glob(f))

    h_files = []
    for h in args.headers:
        h_files.extend(glob.glob(h))

    total_issues = 0

    print("=" * 70)
    print("第 1 层：括号配平与 EXPORT 一致性")
    print("=" * 70)
    for c in c_files:
        issues = check_balance(c)
        if issues:
            total_issues += len(issues)
            print(f"\n❌ {c}")
            for i in issues:
                print(f"   - {i}")
        else:
            print(f"✅ {os.path.basename(c)}")

    print()
    print("=" * 70)
    print("第 2 层：函数符号完整性")
    print("=" * 70)
    for c in c_files:
        unknown = check_symbols(c, h_files)
        if unknown:
            total_issues += len(unknown)
            print(f"\n⚠️  {c} 有 {len(unknown)} 个未知函数调用：")
            for u in unknown[:30]:
                print(f"   - {u}")
            if len(unknown) > 30:
                print(f"   ... 还有 {len(unknown) - 30} 个")
        else:
            print(f"✅ {os.path.basename(c)} 全部函数可解析")

    print()
    print("=" * 70)
    print("第 3 层：内核 API 检查")
    print("=" * 70)
    for c in c_files:
        susp = check_kernel_api(c)
        if susp:
            print(f"\n⚠️  {c} 使用了 stub 未声明的内核 API：")
            for s in susp[:20]:
                print(f"   - {s}")
        else:
            print(f"✅ {os.path.basename(c)} 内核 API 全部在已知列表内")

    print()
    print("=" * 70)
    print("第 4 层：宏一致性")
    print("=" * 70)
    results = check_macro_consistency(c_files, h_files)
    for c, missing in results.items():
        if missing:
            total_issues += len(missing)
            print(f"\n⚠️  {c} 引用了未定义的宏：")
            for m in missing[:20]:
                print(f"   - {m}")
        else:
            print(f"✅ {os.path.basename(c)} 宏全部有定义")

    if not args.skip_syntax:
        print()
        print("=" * 70)
        print("第 5 层：C 语法解析（pycparser）")
        print("=" * 70)
        for c in c_files:
            ok, err = check_syntax(c)
            if ok:
                print(f"✅ {os.path.basename(c)} 语法解析通过")
            else:
                total_issues += 1
                print(f"\n❌ {c} 语法错误：")
                print(f"   {err[:500]}")

    print()
    print("=" * 70)
    if total_issues == 0:
        print("🎉 全部检查通过")
    else:
        print(f"⚠️  共发现 {total_issues} 个问题，需逐个确认")
    print("=" * 70)
    return 0 if total_issues == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
