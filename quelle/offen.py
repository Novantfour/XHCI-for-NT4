#!/usr/bin/env python3
"""
offen.py -- zweite, lexikalische Messung.

Der Uebersetzerweg (messen_gcc.sh) bricht bei grossen Treibern zusammen:
sobald ein Typ unbekannt ist, laesst gcc alles dahinter fallen und die
Zahl faellt zu niedrig aus.  Diese Messung zaehlt stattdessen jeden
Bezeichner, den der Treiber benutzt und der weder in seinem eigenen
Verzeichnis noch in den echten Kopfdateien des Untersystems definiert
ist.  Das ist die Obergrenze; die Wahrheit liegt dazwischen.

    offen.py <echt-verz> <quellverz> [datei...]
"""
import re, sys, os, collections

IDENT = re.compile(r'\b[A-Za-z_][A-Za-z0-9_]*\b')
# Was als "definiert" gilt, wenn es irgendwo im eigenen Baum vorkommt
DEF = re.compile(
    r'^\s*#\s*define\s+([A-Za-z_]\w*)'
    r'|\btypedef\b[^;]*?\b([A-Za-z_]\w*)\s*;'
    r'|^\s*(?:static\s+|inline\s+|extern\s+|const\s+|unsigned\s+|struct\s+|enum\s+|union\s+|void\s+|int\s+|u\d+\s+|__\w+\s+)*'
    r'([A-Za-z_]\w*)\s*\('
    r'|^\s*(?:struct|union|enum)\s+([A-Za-z_]\w*)',
    re.M)

KEYWORDS = set('''auto break case char const continue default do double else enum extern
float for goto if inline int long register restrict return short signed sizeof static
struct switch typedef union unsigned void volatile while _Bool _Generic asm __asm__
__inline__ __attribute__ __extension__ __typeof__ typeof'''.split())

def sammle_defs(pfade):
    d = set()
    for p in pfade:
        for root, _, files in os.walk(p):
            for f in files:
                if f.endswith(('.c', '.h')):
                    txt = open(os.path.join(root, f), errors='ignore').read()
                    for m in DEF.finditer(txt):
                        for g in m.groups():
                            if g:
                                d.add(g)
                    # enum-Werte
                    for m in re.finditer(r'enum\s*\w*\s*\{([^}]*)\}', txt, re.S):
                        for e in IDENT.findall(m.group(1)):
                            d.add(e)
                    # Strukturfelder und lokale Variablen grob
                    for m in re.finditer(r'\b([a-z_]\w*)\s*(?:\[[^\]]*\])?\s*;', txt):
                        d.add(m.group(1))
    return d

echt, quell = sys.argv[1], sys.argv[2]
dateien = sys.argv[3:] or [f for f in os.listdir(quell) if f.endswith('.c')]

bekannt = sammle_defs([echt, quell]) | KEYWORDS

benutzt = collections.Counter()
for f in dateien:
    txt = open(os.path.join(quell, f), errors='ignore').read()
    txt = re.sub(r'/\*.*?\*/', ' ', txt, flags=re.S)
    txt = re.sub(r'//[^\n]*', ' ', txt)
    txt = re.sub(r'"(\\.|[^"\\])*"', ' ', txt)
    for i in IDENT.findall(txt):
        benutzt[i] += 1

offen = {k: v for k, v in benutzt.items() if k not in bekannt}

gebiete = [
    ("Speicher",            r'^(k[mzcv]alloc|kfree|vmalloc|vfree|devm_|dma_pool|GFP_)'),
    ("Sperren und Warten",  r'^(spin_|raw_spin|mutex_|msleep|udelay|mdelay|ndelay|usleep|wait_|complete|schedule|might_sleep|down|up$|sema)'),
    ("PCI",                 r'^(pci_|PCI_|pcim_)'),
    ("DMA",                 r'^(dma_|DMA_|sg_|scatterlist)'),
    ("Register und io",     r'^(read[bwlq]|write[bwlq]|ioread|iowrite|ioremap|iounmap|io[a-z]*_|in[bwl]$|out[bwl]$)'),
    ("Interrupt",           r'^(request_irq|free_irq|irq|IRQ|IRQF|synchronize_irq|in_interrupt|local_irq)'),
    ("Geraetemodell",       r'^(dev_|device_|driver_|put_device|get_device|of_|platform_|sysfs|debugfs|attribute)'),
    ("Energieverwaltung",   r'^(pm_|PM_|SET_|runtime_|suspend|resume|wakeup)'),
    ("Zeit und Timer",      r'^(jiffies|timer_|del_timer|mod_timer|hrtimer|ktime|time_|HZ$|NSEC|USEC|MSEC|CLOCK_)'),
    ("Arbeitswarteschl.",   r'^(work|INIT_WORK|queue_work|flush_work|tasklet|kthread|workqueue)'),
    ("Fehlerwerte",         r'^(E[A-Z]+$|IS_ERR|PTR_ERR|ERR_PTR|WARN|BUG|panic)'),
    ("Makros und CONFIG",   r'^(CONFIG_|ARRAY_SIZE|BIT$|BIT_ULL|GENMASK|likely|unlikely|min$|max$|min_t|max_t|container_of|offsetof|DIV_ROUND|__|round)'),
    ("Listen und Bitfelder",r'^(list_|hlist_|INIT_LIST|set_bit|clear_bit|test_bit|test_and|find_first|bitmap|fls|ffs|hweight)'),
    ("Zeichenketten",       r'^(str[a-z]*|mem[a-z]*|snprintf|sprintf|sscanf|printk|pr_|KERN_)'),
    ("Modulgeruest",        r'^(MODULE_|module_|EXPORT_SYMBOL|__init|__exit|late_initcall)'),
]

rest = dict(offen)
print(f"benutzte, nicht im eigenen Baum definierte Bezeichner: {len(offen)}")
print()
for name, muster in gebiete:
    r = re.compile(muster)
    treffer = [k for k in rest if r.match(k)]
    if treffer:
        print(f"   {name:<24} {len(treffer):4d}")
    for k in treffer:
        del rest[k]
print(f"   {'sonstiges':<24} {len(rest):4d}")
print()
print("sonstiges, nach Haeufigkeit:")
for k, v in sorted(rest.items(), key=lambda x: -x[1])[:40]:
    print(f"     {k:<34} {v}")
