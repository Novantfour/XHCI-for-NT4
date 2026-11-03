/*
 *  sprache.h -- Deutsch, Englisch, Franzoesisch nach eingestellter Region.
 *
 *  Die Sprache kommt aus GetUserDefaultLangID() (Systemsteuerung ->
 *  Laendereinstellungen).  Alles, was nicht Deutsch oder Franzoesisch
 *  ist, bekommt Englisch -- so versteht es jeder.
 *
 *  Benutzung:   S(T_SCHLIESSEN)   liefert den Text in der Sprache des
 *  Benutzers.  Die Tabellen stehen in derselben Reihenfolge wie die
 *  Aufzaehlung; ein fehlender Eintrag faellt auf Englisch zurueck.
 *
 *  Kein Umlaut-Problem: NT 4.0 zeigt die Oberflaeche in der Codepage
 *  des Systems; Deutsch und Franzoesisch liegen beide in 1252.
 */

#ifndef SPRACHE_H
#define SPRACHE_H

enum {
    T_LEER = 0,
    /* Knoepfe und Fenster */
    T_TITEL, T_SCHLIESSEN, T_EIGENSCHAFTEN, T_AKTUALISIEREN,
    T_TREIBER_EINRICHTEN, T_DIAGNOSE, T_PCIIDS,
    /* Geraeteklassen */
    T_K_ANSCHLUESSE, T_K_CDROM, T_K_DISKETTE, T_K_LAUFWERKE, T_K_MAEUSE,
    T_K_PROZESSOREN, T_K_TASTATUREN, T_K_USB, T_K_SYSTEM,
    /* Eigenschaftenseiten */
    T_ALLGEMEIN, T_SCHNITTSTELLEN, T_RESSOURCEN, T_TREIBER,
    T_BEZEICHNUNG, T_HERSTELLER, T_PRODUKT, T_SERIENNUMMER, T_KENNUNG,
    T_GERAETEKLASSE, T_ANSCHLUSS, T_ORT, T_STROMBEDARF, T_AUFBAU,
    T_GERAETETYP, T_KLASSENCODE, T_REVISION, T_ART, T_QUELLE,
    /* USB */
    T_USB_WURZELHUB, T_USB_GERAET, T_USB_HUB, T_KEIN_TREIBER, T_BUS,
    T_ADRESSE, T_TIEFE, T_KEIN_TEXT, T_KONFIGURATIONEN,
    /* Geschwindigkeiten */
    T_LOWSPEED, T_FULLSPEED, T_HIGHSPEED, T_SUPERSPEED, T_UNBEKANNT,
    /* Meldungen */
    T_M_KEIN_PCI, T_M_PCIIDS_FRAGE, T_M_PCIIDS_OK, T_M_PCIIDS_FEHLER,
    T_M_KEINE_DATEI, T_M_GESCHRIEBEN, T_M_NEUSTART,
    /* Installer */
    T_I_TITEL, T_I_EINRICHTEN, T_I_ENTFERNEN, T_I_FERTIG, T_I_PRUEFEN,
    T_I_WEITER, T_I_ABBRECHEN, T_I_LAEUFT,
    /* Reiter in der Systemsteuerung */
    T_R_MAUSTASTEN, T_R_SONDERTASTEN,
    /* Werkzeuge */
    T_W_KEIN_TREIBER, T_W_GERAETE, T_W_HOSTBAUSTEINE, T_W_ZUSTAND,
    T_W_LAEUFT, T_W_GESTOPPT, T_W_AN, T_W_AUS,
    T_ANZAHL
};

static const char *sp_de[T_ANZAHL] = {
    "",
    "Geraetemanager", "Schliessen", "Eigenschaften", "Aktualisieren",
    "Treiber einrichten", "Diagnose", "PCI.IDS...",
    "Anschluesse (COM und LPT)", "DVD/CD-ROM-Laufwerke", "Diskettenlaufwerke",
    "Laufwerke", "Maeuse und andere Zeigegeraete",
    "Prozessoren", "Tastaturen", "USB-Geraete", "Systemgeraete",
    "Allgemein", "Schnittstellen", "Ressourcen", "Treiber",
    "Bezeichnung:", "Hersteller:", "Produkt:", "Seriennummer:", "Kennung:",
    "Geraeteklasse:", "Anschluss:", "Ort:", "Strombedarf:", "Aufbau:",
    "Geraetetyp:", "Klassencode:", "Revision:", "Art:", "Quelle:",
    "USB-Wurzelhub", "USB-Geraet", "USB-Hub", "kein Treiber", "Bus",
    "Adresse", "Tiefe", "(kein Text)", "Konfiguration(en)",
    "Low Speed (1,5 MBit/s)", "Full Speed (12 MBit/s)",
    "High Speed (480 MBit/s)", "SuperSpeed (5 GBit/s)", "unbekannt",
    /* Meldungen */
    "Der PCI-Hilfstreiber (PCISCAN) ist nicht eingerichtet -- der PCI-Zweig bleibt leer.",
    "PCI.IDS aktualisieren?\r\n\r\nJa = aus dem Internet\r\nNein = aus einer Datei\r\nAbbrechen = nichts aendern",
    "PCI.IDS aktualisiert.", "Laden fehlgeschlagen:",
    "Datei nicht gefunden.", "Geschrieben:", "Bitte neu starten.",
    /* Installer */
    "USB-Treiber einrichten", "Einrichten", "Entfernen", "Fertig.",
    "Pruefen", "Weiter", "Abbrechen", "laeuft...",
    "USB-Maustasten", "Sondertasten",
    "USB-Treiber laeuft nicht.", "Geraete", "Hostbausteine", "Zustand", "laeuft", "gestoppt", "an", "aus"
};

static const char *sp_en[T_ANZAHL] = {
    "",
    "Device Manager", "Close", "Properties", "Refresh",
    "Install driver", "Diagnostics", "PCI.IDS...",
    "Ports (COM & LPT)", "DVD/CD-ROM drives", "Floppy disk drives",
    "Disk drives", "Mice and other pointing devices",
    "Processors", "Keyboards", "USB devices", "System devices",
    "General", "Interfaces", "Resources", "Driver",
    "Name:", "Manufacturer:", "Product:", "Serial number:", "Hardware ID:",
    "Device class:", "Connection:", "Location:", "Power:", "Layout:",
    "Device type:", "Class code:", "Revision:", "Type:", "Source:",
    "USB root hub", "USB device", "USB hub", "no driver", "Bus",
    "Address", "Depth", "(no string)", "configuration(s)",
    "Low Speed (1.5 Mbit/s)", "Full Speed (12 Mbit/s)",
    "High Speed (480 Mbit/s)", "SuperSpeed (5 Gbit/s)", "unknown",
    /* Messages */
    "The PCI helper driver (PCISCAN) is not installed -- the PCI branch stays empty.",
    "Update PCI.IDS?\r\n\r\nYes = from the Internet\r\nNo = from a file\r\nCancel = leave unchanged",
    "PCI.IDS updated.", "Download failed:",
    "File not found.", "Written:", "Please restart.",
    /* Installer */
    "Install USB driver", "Install", "Remove", "Done.",
    "Check", "Next", "Cancel", "running...",
    "USB mouse buttons", "Special keys",
    "USB driver is not running.", "Devices", "Host controllers", "State", "running", "stopped", "on", "off"
};

static const char *sp_fr[T_ANZAHL] = {
    "",
    "Gestionnaire de peripheriques", "Fermer", "Proprietes", "Actualiser",
    "Installer le pilote", "Diagnostic", "PCI.IDS...",
    "Ports (COM et LPT)", "Lecteurs de DVD/CD-ROM", "Lecteurs de disquettes",
    "Lecteurs de disque", "Souris et autres peripheriques de pointage",
    "Processeurs", "Claviers", "Peripheriques USB", "Peripheriques systeme",
    "General", "Interfaces", "Ressources", "Pilote",
    "Nom :", "Fabricant :", "Produit :", "Numero de serie :", "Identifiant :",
    "Classe :", "Connexion :", "Emplacement :", "Alimentation :", "Structure :",
    "Type de peripherique :", "Code de classe :", "Revision :", "Type :", "Source :",
    "Concentrateur racine USB", "Peripherique USB", "Concentrateur USB",
    "aucun pilote", "Bus",
    "Adresse", "Profondeur", "(aucun texte)", "configuration(s)",
    "Low Speed (1,5 Mbit/s)", "Full Speed (12 Mbit/s)",
    "High Speed (480 Mbit/s)", "SuperSpeed (5 Gbit/s)", "inconnu",
    /* Messages */
    "Le pilote auxiliaire PCI (PCISCAN) n'est pas installe -- la branche PCI reste vide.",
    "Mettre a jour PCI.IDS ?\r\n\r\nOui = depuis Internet\r\nNon = depuis un fichier\r\nAnnuler = ne rien changer",
    "PCI.IDS mis a jour.", "Echec du telechargement :",
    "Fichier introuvable.", "Ecrit :", "Veuillez redemarrer.",
    /* Installeur */
    "Installer le pilote USB", "Installer", "Supprimer", "Termine.",
    "Verifier", "Suivant", "Annuler", "en cours...",
    "Boutons souris USB", "Touches speciales",
    "Le pilote USB ne fonctionne pas.", "Peripheriques", "Controleurs hote", "Etat", "actif", "arrete", "oui", "non"
};

/*  LANG_* aus winnt.h: Deutsch 0x07, Franzoesisch 0x0C.  Nur die
    Hauptsprache zaehlt (die unteren 10 Bit), damit auch Schweizer oder
    kanadisches Franzoesisch passt.  */
static const char **sp_tabelle(void)
{
    static const char **t;
    if (!t) {
        WORD s = (WORD)(GetUserDefaultLangID() & 0x3FF);
        t = (s == 0x07) ? sp_de : (s == 0x0C) ? sp_fr : sp_en;
    }
    return t;
}

static const char *S(int id)
{
    const char **t = sp_tabelle();
    if (id <= 0 || id >= T_ANZAHL) return "";
    return (t[id] && t[id][0]) ? t[id] : sp_en[id];
}

#endif
