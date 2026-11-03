#ifndef _SYS_PARAM_H_
#define _SYS_PARAM_H_
#include <sys/types.h>
#define __FreeBSD_version 1400000
#define PAGE_SIZE   4096
#define PAGE_SHIFT  12
#define PAGE_MASK   (PAGE_SIZE - 1)
/*  hz und ticks sind in FreeBSD echte Variablen, keine Makros.  Als
 *  Makro wuerden sie jeden gleichnamigen Parameter im Treiber
 *  ueberschreiben -- der Syntaxlauf hat genau das gefunden, bevor es
 *  auf die Maschine kam.  hz ist 100, weil NT 4.0 seinen Takt bei
 *  rund zehn Millisekunden hat; 1000 zu behaupten waere eine Luege,
 *  die erst bei den Zeitschranken auffiele.  */
extern const int hz;
#define MAXPHYS     (128 * 1024)
#define howmany(x,y)   (((x) + ((y) - 1)) / (y))
#define rounddown(x,y) (((x) / (y)) * (y))
#define roundup(x,y)   ((((x) + ((y) - 1)) / (y)) * (y))
#define roundup2(x,y)  (((x) + ((y) - 1)) & (~((y) - 1)))
#define rounddown2(x,y) ((x) & (~((y) - 1)))
#define powerof2(x)    ((((x) - 1) & (x)) == 0)
#ifndef MIN
#define MIN(a,b) (((a) < (b)) ? (a) : (b))
#define MAX(a,b) (((a) > (b)) ? (a) : (b))
#endif
#define nitems(x)   (sizeof((x)) / sizeof((x)[0]))
#define __offsetof(t,f) ((size_t)(&((t *)0)->f))
#define offsetof(t,f)   __offsetof(t,f)
/*  Diese beiden waren zuerst leer.  Der Uebersetzungslauf hat es
 *  gefunden, und zwar an der Zusicherung, die FreeBSD selbst
 *  mitbringt:
 *
 *      extern char usb_device_id_assert[
 *          (sizeof(struct usb_device_id) == 32) ? 1 : -1];
 *
 *  struct usb_device_id ist __aligned(32).  Mit leerem __aligned wird
 *  sie 20 Byte gross, die Zusicherung schlaegt fehl -- und haette sie
 *  nicht dort gestanden, waere jede Suche in einer Geraetetabelle mit
 *  falscher Schrittweite gelaufen.
 *
 *  __packed ist der schlimmere Fall: die USB-Deskriptoren sind alle
 *  gepackt.  Leer gelassen schiebt der Uebersetzer Fuellbytes ein, und
 *  dann liest der Stapel jedes Geraet falsch aus -- ohne dass irgendwo
 *  ein Fehlerbit steht.  Genau die Falle "Strukturlagen" aus dem Skill.
 *
 *  clang-cl nimmt die GNU-Schreibweise an, solange -fms-extensions
 *  gesetzt ist; __declspec(align) taugt hier nicht, weil es hinter dem
 *  Rumpf stehen muesste.  */
#define __packed      __attribute__((packed))
#define __aligned(n)  __attribute__((aligned(n)))
#define __unused
#define __predict_true(e)  (e)
#define __predict_false(e) (e)
#define __printflike(a,b)
#endif
