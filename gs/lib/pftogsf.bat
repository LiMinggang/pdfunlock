@echo off

rem
rem ******************************
rem * Convert .pf? files to .gsf *
rem ******************************
rem
echo (wrfont.ps) run (unprot.ps) run unprot >_temp_.ps
echo systemdict /definefont. /definefont load put >>_temp_.ps
echo systemdict /definefont { userdict /LFN 3 index put definefont. } bind put >>_temp_.ps
echo ARGUMENTS 0 get (r) file .loadfont LFN findfont setfont prunefont reprot >>_temp_.ps
echo ARGUMENTS 1 get (w) file dup writefont closefile quit >>_temp_.ps
rem for %%f in (cyr cyri) do gs -q -dNODISPLAY -dWRITESYSTEMDICT -- _temp_.ps fonts\pfa\%%f.pfa fonts\%%f.gsf
rem for %%f in (ncrr ncrb ncrri ncrbi) do gs -q -dNODISPLAY -dWRITESYSTEMDICT -- _temp_.ps fonts\pfa\%%f.pfa fonts\%%f.gsf
rem for %%f in (bchr bchb bchri bchbi) do gs -q -dNODISPLAY -dWRITESYSTEMDICT -- _temp_.ps fonts\pfa\%%f.pfa fonts\%%f.gsf
rem for %%f in (putr putb putri putbi) do gs -q -dNODISPLAY -dWRITESYSTEMDICT -- _temp_.ps fonts\pfa\%%f.pfa fonts\%%f.gsf
rem for %%f in (n019003l n021003l u003043t u004006t) do gs -q -dNODISPLAY -dWRITESYSTEMDICT -- _temp_.ps fonts\%%f.gsf %%f.gsf
for %%f in (hig_____ kak_____) do gs -q -dNODISPLAY -dWRITESYSTEMDICT -- _temp_.ps fonts\pfb\%%f.pfb %%f.gsf
rem gs -q -dNODISPLAY -dWRITESYSTEMDICT -- _temp_.ps allfonts\baxter.pfb baxter.gsf
