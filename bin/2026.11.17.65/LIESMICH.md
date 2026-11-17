# Fassung 2026.11.17.65

Durchsicht .52-.64: Handler laeuft immer gesperrt (auch beim Sicherungstakt), ISR sperrt nach, wenn der Handler die Freigabe wieder oeffnet, INTx aus im Abfragebetrieb, EHCI-SMI immer aus. Gebaut mit dem NT-4.0-DDK. Siehe AENDERUNGEN.md.
