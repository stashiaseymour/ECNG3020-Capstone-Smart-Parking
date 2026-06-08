# ParkSmart – IoT-Based Smart Parking System
**ECNG3020 Capstone Project | University of the West Indies, Mona**

## Project Overview
ParkSmart is a dual-model IoT-based smart parking system designed to automate parking space monitoring and reservation management. The system uses ESP32-S3 microcontrollers paired with VL53L0X time-of-flight sensors for indoor detection and BMM150 magnetic sensors combined with VL53L0X for outdoor detection. Occupancy data is transmitted wirelessly via ESP-NOW to a central gateway, which forwards updates to a cloud-hosted backend in real time. Users can reserve parking spaces, check in via QR code, and monitor live space availability through a web dashboard.

## Repository Structure

| Folder | Description |
|---|---|
| `Indoor_Node/` | ESP32-S3 firmware for indoor parking nodes. Uses the VL53L0X time-of-flight sensor to detect vehicle presence based on distance measurements. |
| `Outdoor_Node/` | ESP32-S3 firmware for outdoor parking nodes. Uses a combination of the BMM150 magnetic field sensor and VL53L0X for dual-mode occupancy detection. |
| `Gateway/` | ESP32-S3 gateway firmware responsible for receiving ESP-NOW packets from all nodes, forwarding sensor data to the backend via HTTPS, and relaying status decisions back to nodes. |
| `Server/` | FastAPI backend connected to a MongoDB database. Handles parking space status computation, reservation management, session billing, and all REST API endpoints. |
| `Webpage/` | Frontend web dashboard built for user-facing interactions including real-time space availability, QR-based check-in, reservations, and admin controls. |

## System Architecture
- **Wireless Communication:** ESP-NOW protocol for low-latency node-to-gateway communication
- **Cloud Communication:** HTTPS REST API between the gateway and backend server
- **Backend Framework:** FastAPI with MongoDB, deployed on an Ubuntu server
- **Frontend Hosting:** Deployed on Netlify
- **Status Logic:** Final space status is determined server-side following a priority chain: MAINTENANCE → VIOLATION → OCCUPIED → RESERVED (checked-in) → RESERVED (booking only) → sensor raw status

## Development Notes
This project was developed as part of the ECNG3020 final year capstone in the Electronics Engineering (Telecommunications) programme. All source code across firmware, backend, and frontend components is organized by subsystem in this repository. Configuration files containing deployment-specific environment variables are excluded and must be set up separately based on the target deployment environment.