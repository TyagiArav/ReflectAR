@echo off
rem Start the ReflectAR perception service (webcam -^> pose/depth on GPU -^> Unity on :5566).
cd /d "%~dp0perception" && .venv\Scripts\python.exe -u server.py --log %*
