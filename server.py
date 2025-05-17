from flask import Flask, request, send_from_directory, make_response
from gtts import gTTS
from pydub import AudioSegment
import wave, os, uuid, requests, json

app = Flask(__name__)
UPLOAD_FOLDER = "audios"
SENSOR_LOG = "sensor_log.txt"
os.makedirs(UPLOAD_FOLDER, exist_ok=True)

GROQ_API_KEY   = "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"
WHISPER_MODEL  = "whisper-large-v3"
CHAT_MODEL     = "llama3-8b-8192"

@app.route("/upload", methods=["POST"])
def upload():
    # 1️⃣ Ses dosyasını kaydet
    file_id = str(uuid.uuid4())
    wav_path = os.path.join(UPLOAD_FOLDER, f"{file_id}.wav")
    with open(wav_path, "wb") as f:
        f.write(request.data)

    # 2️⃣ Ses → Yazı (Whisper)
    headers = {"Authorization": f"Bearer {GROQ_API_KEY}"}
    with open(wav_path, "rb") as af:
        asr_res = requests.post(
            "https://api.groq.com/openai/v1/audio/transcriptions",
            headers=headers,
            files={"file": (os.path.basename(wav_path), af, "audio/wav")},
            data={"model": WHISPER_MODEL}
        )
    asr_res.raise_for_status()
    user_text = asr_res.json().get("text", "").strip()

    # 🛑 Kullanıcıdan geçerli soru yoksa işlemi durdur
    if not user_text:
        print("⚠️ Kullanıcıdan geçerli bir soru algılanamadı. LLM çağrısı yapılmadı.")
        return "Soru algılanamadı.", 204  # No Content

    # 3️⃣ sensor_log.txt dosyasından son veriyi al ve ayrıştır
    if os.path.exists(SENSOR_LOG):
        with open(SENSOR_LOG, "r") as f:
            last_line = f.readlines()[-1].strip()
        try:
            parsed = json.loads(last_line)
            veri_string = (
                f"Hareket: {parsed.get('hareket', 'Yok')}\n"
                f"Sıcaklık: {parsed.get('sicaklik', '?')} °C\n"
                f"Nem: {parsed.get('nem', '?')} %\n"
                f"Basınç: {parsed.get('basinc', '?')} hPa"
            )
        except Exception:
            veri_string = "Veri ayrıştırılamadı."
    else:
        veri_string = "Sensör verisi bulunamadı."

    # 4️⃣ Prompt oluştur
    prompt = (
        f"Kullanıcının sorusu: {user_text}\n\n"
        f"Son sensör verileri:\n{veri_string}\n\n"
        f"Bu verilere göre durumu açıkla."
    )
    print("🧠 LLM'ye gönderilen prompt:\n", prompt)

    # 5️⃣ LLM'den yanıt al
    chat_res = requests.post(
        "https://api.groq.com/openai/v1/chat/completions",
        headers=headers,
        json={
            "model": CHAT_MODEL,
            "messages": [{"role": "user", "content": prompt}]
        }
    )
    chat_res.raise_for_status()
    reply = chat_res.json()["choices"][0]["message"]["content"].strip()

    # ✅ LLM cevabını terminale yazdır
    print("💬 LLM METİN CEVABI:\n", reply)

    # 6️⃣ Cevabı sese çevir (gTTS + WAV)
    mp3_path       = os.path.join(UPLOAD_FOLDER, f"{file_id}.mp3")
    wav_reply_path = os.path.join(UPLOAD_FOLDER, f"{file_id}_reply.wav")
    gTTS(reply, lang="tr").save(mp3_path)

    audio = AudioSegment.from_mp3(mp3_path)
    audio = audio.set_frame_rate(16000).set_channels(1).set_sample_width(2)
    with wave.open(wav_reply_path, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(16000)
        wf.writeframes(audio.raw_data)

    print(f"🔊 Sesli yanıt WAV olarak kaydedildi: {wav_reply_path}")

    # 7️⃣ ESP32'ye URL döndür
    ESP32_IP = "192.168.137.22"
    url = f"http://{ESP32_IP}:5000/audios/{os.path.basename(wav_reply_path)}"
    resp = make_response(url, 200)
    resp.headers["Content-Type"] = "text/plain"
    return resp

@app.route("/audios/<filename>")
def serve_audio(filename):
    return send_from_directory(UPLOAD_FOLDER, filename)

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000)
