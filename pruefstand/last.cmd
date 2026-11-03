@echo off
for %%i in (1 2 3 4 5 6 7 8) do (
  copy /b C:\TEMP\z.bin E:\l%%i.bin > nul
  copy /b E:\l%%i.bin C:\TEMP\r%%i.bin > nul
  fc /b C:\TEMP\z.bin C:\TEMP\r%%i.bin > nul && echo %%i gleich || echo %%i ABWEICHEND
)
echo fertig
