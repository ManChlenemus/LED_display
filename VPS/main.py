import io
import os
import time
import threading
from collections import deque
from dataclasses import dataclass

import requests
from fastapi import FastAPI, HTTPException, Query
from fastapi.responses import Response, PlainTextResponse
from pydantic import BaseModel
from PIL import Image, ImageDraw


BOT_TOKEN = os.environ["BOT_TOKEN"]
DEVICE_KEY = os.environ["DEVICE_KEY"]


ALLOWED_CHAT_ID = os.environ.get("ALLOWED_CHAT_ID", "")

TELEGRAM_API = f"https://api.telegram.org/bot{BOT_TOKEN}"


IMAGE_WIDTH = 36
IMAGE_HEIGHT = 24
IMAGE_BYTES = IMAGE_WIDTH * IMAGE_HEIGHT * 3


app = FastAPI()

state_lock = threading.Lock()


@dataclass
class CommandItem:
    command_id: int
    chat_id: str
    text: str


command_queue = deque()
pending_commands: dict[int, CommandItem] = {}

last_update_id = 0
next_command_id = 1

latest_image_bytes: bytes | None = None
latest_image_id = 0

cached_info_text: str = "Fetching info..."

def fetch_info_loop() -> None:
    global cached_info_text
    print("Info fetching loop started", flush=True)

    while True:
        try:
            temp = "--"
            usd = "--"
            eur = "--"
            btc = "--"

            try:
                res = requests.get("http://api.open-meteo.com/v1/forecast?latitude=55.7512&longitude=37.6184&current_weather=true", timeout=10)
                if res.status_code == 200:
                    t = res.json().get("current_weather", {}).get("temperature")
                    if t is not None:
                        temp = f"+{t:.1f}" if t > 0 else f"{t:.1f}"
            except Exception as e:
                print(f"Weather error: {e}")

            try:
                res = requests.get("https://open.er-api.com/v6/latest/USD", timeout=10)
                if res.status_code == 200:
                    r = res.json().get("rates", {}).get("RUB")
                    if r is not None:
                        usd = f"{r:.1f}"
            except Exception as e:
                print(f"USD error: {e}")

            try:
                res = requests.get("https://open.er-api.com/v6/latest/EUR", timeout=10)
                if res.status_code == 200:
                    r = res.json().get("rates", {}).get("RUB")
                    if r is not None:
                        eur = f"{r:.1f}"
            except Exception as e:
                print(f"EUR error: {e}")

            try:
                res = requests.get("https://api.binance.com/api/v3/ticker/price?symbol=BTCUSDT", timeout=10)
                if res.status_code == 200:
                    price = float(res.json().get("price", 0))
                    if price > 0:
                        btc = f"{int(price)}"
            except Exception as e:
                print(f"BTC error: {e}")

            new_text = f"MSK:{temp}C | USD:{usd} | EUR:{eur} | BTC:${btc}"
            
            with state_lock:
                cached_info_text = new_text

        except Exception as error:
            print(f"fetch_info_loop error: {error}", flush=True)

        time.sleep(900)



class ResultPayload(BaseModel):
    key: str
    command_id: int
    result: str


def telegram_send_message(chat_id: str, text: str) -> None:
    try:
        requests.post(
            f"{TELEGRAM_API}/sendMessage",
            json={
                "chat_id": chat_id,
                "text": text,
            },
            timeout=10,
        )
    except Exception as error:
        print(f"sendMessage error: {error}", flush=True)


def telegram_get_file_path(file_id: str) -> str:
    response = requests.get(
        f"{TELEGRAM_API}/getFile",
        params={"file_id": file_id},
        timeout=15,
    )

    data = response.json()

    if not data.get("ok"):
        raise RuntimeError(f"getFile failed: {data}")

    return data["result"]["file_path"]


def telegram_download_file(file_path: str) -> bytes:
    url = f"https://api.telegram.org/file/bot{BOT_TOKEN}/{file_path}"

    response = requests.get(url, timeout=30)
    response.raise_for_status()

    return response.content


def is_allowed_chat(chat_id: str) -> bool:
    if not ALLOWED_CHAT_ID:
        return True

    return chat_id == ALLOWED_CHAT_ID


def queue_command(chat_id: str, text: str) -> int:
    global next_command_id

    with state_lock:
        command = CommandItem(
            command_id=next_command_id,
            chat_id=chat_id,
            text=text,
        )

        next_command_id += 1

        command_queue.append(command)
        pending_commands[command.command_id] = command

        return command.command_id


def image_to_matrix_rgb(image: Image.Image) -> bytes:
    image = image.convert("RGB")

    source_width, source_height = image.size
    target_ratio = IMAGE_WIDTH / IMAGE_HEIGHT
    source_ratio = source_width / source_height

    if source_ratio > target_ratio:
        new_width = int(source_height * target_ratio)
        left = (source_width - new_width) // 2
        image = image.crop((left, 0, left + new_width, source_height))
    else:
        new_height = int(source_width / target_ratio)
        top = (source_height - new_height) // 2
        image = image.crop((0, top, source_width, top + new_height))

    image = image.resize((IMAGE_WIDTH, IMAGE_HEIGHT), Image.Resampling.LANCZOS)

    return image.tobytes()


def store_image(image_bytes: bytes) -> int:
    global latest_image_bytes, latest_image_id

    with state_lock:
        latest_image_bytes = image_bytes
        latest_image_id += 1
        return latest_image_id


def create_test_gradient() -> bytes:
    image = Image.new("RGB", (IMAGE_WIDTH, IMAGE_HEIGHT), "black")
    draw = ImageDraw.Draw(image)

    for y in range(IMAGE_HEIGHT):
        for x in range(IMAGE_WIDTH):
            r = int(255 * x / (IMAGE_WIDTH - 1))
            g = int(255 * y / (IMAGE_HEIGHT - 1))
            b = 120
            draw.point((x, y), fill=(r, g, b))

    return image.tobytes()


def handle_telegram_photo(chat_id: str, message: dict) -> None:
    photos = message.get("photo", [])

    if not photos:
        return

    largest_photo = photos[-1]
    file_id = largest_photo["file_id"]

    try:
        file_path = telegram_get_file_path(file_id)
        raw_file = telegram_download_file(file_path)

        image = Image.open(io.BytesIO(raw_file))
        matrix_bytes = image_to_matrix_rgb(image)

        if len(matrix_bytes) != IMAGE_BYTES:
            raise RuntimeError(f"Invalid image byte size: {len(matrix_bytes)}")

        image_id = store_image(matrix_bytes)        
        queue_command(chat_id, "/image")

        telegram_send_message(
            chat_id,
            f"Photo received.\nImage id: {image_id}"
        )

        print(
            f"photo stored image_id={image_id} bytes={len(matrix_bytes)} chat_id={chat_id}",
            flush=True,
        )

    except Exception as error:
        print(f"photo handling error: {error}", flush=True)
        telegram_send_message(chat_id, f"Photo processing failed:\n{error}")


def telegram_polling_loop() -> None:
    global last_update_id

    print("Telegram polling started", flush=True)

    while True:
        try:
            response = requests.get(
                f"{TELEGRAM_API}/getUpdates",
                params={
                    "offset": last_update_id + 1,
                    "timeout": 25,
                },
                timeout=35,
            )

            data = response.json()

            if not data.get("ok"):
                print(f"Telegram getUpdates not ok: {data}", flush=True)
                time.sleep(3)
                continue

            for update in data.get("result", []):
                last_update_id = update["update_id"]

                message = update.get("message")
                if not message:
                    continue

                chat_id = str(message["chat"]["id"])

                print(
                    f"chat_id={chat_id} message_keys={list(message.keys())}", flush=True
                )

                if not is_allowed_chat(chat_id):
                    telegram_send_message(chat_id, "Access denied")
                    continue

                if "photo" in message:
                    handle_telegram_photo(chat_id, message)
                    continue

                text = message.get("text", "").strip()

                if not text:
                    continue

                print(f"chat_id={chat_id} text={text}", flush=True)

                if text == "/chatid":
                    telegram_send_message(chat_id, f"Your chat_id:\n{chat_id}")
                    continue

                command_id = queue_command(chat_id, text)

                telegram_send_message(
                    chat_id,
                    f"Queued command #{command_id}:\n{text}",
                )

        except Exception as error:
            print(f"polling error: {error}", flush=True)
            time.sleep(3)


@app.get("/health")
def health():
    with state_lock:
        return {
            "ok": True,
            "queue_size": len(command_queue),
            "pending_size": len(pending_commands),
            "allowed_chat_id_set": bool(ALLOWED_CHAT_ID),
            "has_image": latest_image_bytes is not None,
            "image_id": latest_image_id,
            "image_bytes": len(latest_image_bytes) if latest_image_bytes else 0,
        }


@app.get("/esp/command")
def get_command(key: str = Query(...)):
    if key != DEVICE_KEY:
        raise HTTPException(status_code=403, detail="Invalid key")

    with state_lock:
        if not command_queue:
            return {
                "has_command": False,
            }

        command = command_queue.popleft()

        return {
            "has_command": True,
            "command_id": command.command_id,
            "text": command.text,
        }


@app.post("/esp/result")
def post_result(payload: ResultPayload):
    if payload.key != DEVICE_KEY:
        raise HTTPException(status_code=403, detail="Invalid key")

    with state_lock:
        command = pending_commands.pop(payload.command_id, None)

    if not command:
        return {
            "ok": False,
            "error": "Unknown command_id",
        }

    telegram_send_message(
        command.chat_id,
        f"Result for command #{payload.command_id}:\n{payload.result}",
    )

    return {
        "ok": True,
    }


@app.post("/test/image")
def create_test_image(key: str = Query(...)):
    if key != DEVICE_KEY:
        raise HTTPException(status_code=403, detail="Invalid key")

    image_bytes = create_test_gradient()
    image_id = store_image(image_bytes)

    return {
        "ok": True,
        "image_id": image_id,
        "bytes": len(image_bytes),
        "width": IMAGE_WIDTH,
        "height": IMAGE_HEIGHT,
    }


@app.get("/esp/image/meta")
def get_image_meta(key: str = Query(...)):
    if key != DEVICE_KEY:
        raise HTTPException(status_code=403, detail="Invalid key")

    with state_lock:
        return {
            "has_image": latest_image_bytes is not None,
            "image_id": latest_image_id,
            "width": IMAGE_WIDTH,
            "height": IMAGE_HEIGHT,
            "bytes": len(latest_image_bytes) if latest_image_bytes else 0,
        }


@app.get("/esp/image/data")
def get_image_data(key: str = Query(...)):
    if key != DEVICE_KEY:
        raise HTTPException(status_code=403, detail="Invalid key")

    with state_lock:
        if latest_image_bytes is None:
            raise HTTPException(status_code=404, detail="No image")

        image_bytes = latest_image_bytes

    return Response(
        content=image_bytes,
        media_type="application/octet-stream",
    )


@app.get("/esp/info")
def get_info(key: str = Query(...)):
    if key != DEVICE_KEY:
        raise HTTPException(status_code=403, detail="Invalid key")
    
    with state_lock:
        return PlainTextResponse(content=cached_info_text)


threading.Thread(target=telegram_polling_loop, daemon=True).start()
threading.Thread(target=fetch_info_loop, daemon=True).start()
