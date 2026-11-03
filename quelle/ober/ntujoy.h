/*
 *  ober/ntujoy.h -- Gamepads und Joysticks: Kern <-> usbjoy.dll
 *
 *  Der Kerntreiber (ober/ntujoy.c) liest die Berichte eines Gamepads
 *  fortlaufend und haelt den AKTUELLEN Zustand bereit.  usbjoy.dll -- der
 *  Joystick-Treiber fuer winmm (joyGetPosEx) -- fragt nur diesen Zustand
 *  ab und weiss nichts von USB.  So ist joyGetPosEx schnell: kein Warten
 *  auf einen Bericht, nur ein Kopieren.
 *
 *      \\.\UsbJoy0 .. \\.\UsbJoy15      je Gamepad
 *
 *  Achsen sind auf 0..65535 gebracht, Mitte 32768 -- wie winmm es fuer
 *  JOYCAPS wXmin/wXmax erwartet.  Y waechst nach UNTEN (winmm-Sitte);
 *  beim Xbox-Controller wird deshalb Y umgedreht, der meldet "oben =
 *  positiv".
 */

#ifndef _NTUJOY_H_
#define _NTUJOY_H_

#define NTUJ_FASSUNG     1
#define NTUJ_ACHSEN      8
#define NTUJ_POV_MITTE   0xFFFF          /* = JOY_POVCENTERED */

/*  Achsenart: die HID-Verwendung (Generic Desktop).  */
#define NTUJ_X       0x30
#define NTUJ_Y       0x31
#define NTUJ_Z       0x32
#define NTUJ_RX      0x33
#define NTUJ_RY      0x34
#define NTUJ_RZ      0x35
#define NTUJ_SLIDER  0x36
#define NTUJ_DIAL    0x37
#define NTUJ_WHEEL   0x38

#define NTUJ_IOCTL(n)  (((0x00000022) << 16) | ((0x8B0 + (n)) << 2))  /* FILE_DEVICE_UNKNOWN, METHOD_BUFFERED */

#define IOCTL_NTUJ_INFO     NTUJ_IOCTL(0)
#define IOCTL_NTUJ_ZUSTAND  NTUJ_IOCTL(1)
#define IOCTL_NTUJ_RUETTELN NTUJ_IOCTL(2)    /* Vibration: Xbox 360, Xbox One/Series */

/*  IOCTL_NTUJ_RUETTELN: Staerke der beiden Motoren, 0 = aus, 255 = voll.
 *  Bleibt an, bis ein neuer Wert kommt (oder der Controller es selbst
 *  beendet -- Xbox One nach gut 2,5 s).  Andere Gamepads: nicht
 *  unterstuetzt (ERROR_NOT_SUPPORTED).  */
typedef struct _NTUJ_RUETTELN {
    unsigned char Stark;                 /* linker, grosser Motor */
    unsigned char Schwach;               /* rechter, kleiner Motor */
    unsigned char Frei[2];
} NTUJ_RUETTELN;

typedef struct _NTUJ_INFO {
    unsigned long   Fassung;             /* NTUJ_FASSUNG */
    unsigned long   AnzahlAchsen;
    unsigned short  AchsenArt[NTUJ_ACHSEN];
    unsigned long   AnzahlKnoepfe;       /* hoechste belegte Knopfnummer */
    unsigned long   HatPov;              /* Hutschalter oder Steuerkreuz */
    unsigned short  Vid, Pid;
    unsigned long   Xbox;                /* 1 = Xbox-360-Controller */
    char            Name[32];
} NTUJ_INFO;

typedef struct _NTUJ_ZUSTAND {
    unsigned long   Fassung;
    unsigned long   Zaehler;             /* Berichte seit dem Einstecken */
    unsigned long   Achse[NTUJ_ACHSEN];  /* 0..65535, in der Reihenfolge von Info */
    unsigned long   Knoepfe;             /* Bit 0 = Knopf 1 */
    unsigned long   Pov;                 /* Hundertstel Grad, NTUJ_POV_MITTE */
} NTUJ_ZUSTAND;

#endif
