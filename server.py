from flask import Flask, request, send_from_directory, make_response
from gtts import gTTS
from pydub import AudioSegment
import wave, os, uuid, requests, json
import io
import numpy as np
import gc
import threading
import time
import shutil
import psutil

app = Flask(__name__)
UPLOAD_FOLDER = "audios"
SENSOR_LOG = "sensor_log.txt"
os.makedirs(UPLOAD_FOLDER, exist_ok=True)

GROQ_API_KEY   = "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"
WHISPER_MODEL  = "whisper-large-v3"
CHAT_MODEL     = "llama3-8b-8192"

# Aktif kayıtları tutmak için sözlük ve kilit
active_recordings = {}
recordings_lock = threading.Lock()

# Temizlik zamanlayıcısı
def cleanup_timer():
    while True:
        try:
            clean_old_files()
            clean_old_recordings()
        except Exception as e:
            print(f"❌ Temizlik hatası: {str(e)}")
        time.sleep(3600)  # Her saat başı

def clean_old_recordings():
    """30 dakikadan eski kayıtları temizle"""
    try:
        with recordings_lock:
            current_time = time.time()
            to_delete = []
            for session_id, rec in active_recordings.items():
                if hasattr(rec, 'timestamp') and (current_time - rec.timestamp) > 1800:
                    to_delete.append(session_id)
            
            for session_id in to_delete:
                del active_recordings[session_id]
                print(f"🗑️ Eski oturum temizlendi: {session_id}")
            
            gc.collect()
    except Exception as e:
        print(f"❌ Kayıt temizleme hatası: {str(e)}")

def check_disk_space():
    """Disk alanı kontrolü"""
    try:
        disk = psutil.disk_usage(os.path.dirname(UPLOAD_FOLDER))
        free_gb = disk.free / (1024 * 1024 * 1024)
        print(f"💾 Boş disk alanı: {free_gb:.2f}GB")
        
        if free_gb < 0.5:  # 500MB'dan az boş alan varsa
            print(f"⚠️ Kritik disk alanı: {free_gb:.2f}GB")
            clean_old_files(aggressive=True)
            # Tekrar kontrol et
            disk = psutil.disk_usage(os.path.dirname(UPLOAD_FOLDER))
            free_gb = disk.free / (1024 * 1024 * 1024)
            
        return free_gb >= 0.1  # 100MB minimum gerekli alan
    except Exception as e:
        print(f"❌ Disk kontrolü hatası: {str(e)}")
        return True  # Hata durumunda devam et

def clean_old_files(aggressive=False):
    """Eski dosyaları temizle"""
    try:
        current_time = time.time()
        deleted_count = 0
        threshold = 300 if aggressive else 3600  # Agresif modda 5 dakika, normal modda 1 saat
        
        files = []
        for filename in os.listdir(UPLOAD_FOLDER):
            filepath = os.path.join(UPLOAD_FOLDER, filename)
            try:
                mtime = os.path.getmtime(filepath)
                files.append((mtime, filepath, filename))
            except Exception as e:
                print(f"❌ Dosya bilgisi alınamadı ({filename}): {str(e)}")
        
        # Dosyaları tarihe göre sırala (en eski önce)
        files.sort()
        
        # En eski dosyalardan başlayarak temizle
        for mtime, filepath, filename in files:
            try:
                if mtime < (current_time - threshold):
                    os.remove(filepath)
                    deleted_count += 1
                    print(f"🗑️ Dosya silindi: {filename}")
            except Exception as e:
                print(f"❌ Dosya silme hatası ({filename}): {str(e)}")
        
        if deleted_count > 0:
            print(f"✅ {deleted_count} dosya temizlendi")
            
    except Exception as e:
        print(f"❌ Temizleme hatası: {str(e)}")

def validate_wav_data(data):
    """WAV verisini doğrula ve düzelt"""
    try:
        # İlk chunk için WAV header kontrolü
        if len(data) > 44 and data[0:4] == b'RIFF':
            print("✅ WAV header bulundu")
            return data
        
        # Header yoksa ekle
        print("⚠️ WAV header eksik, ekleniyor...")
        header = bytearray(44)
        # RIFF header
        header[0:4] = b'RIFF'
        # Chunk size (placeholder)
        size = len(data) + 36
        header[4:8] = size.to_bytes(4, 'little')
        # Format
        header[8:12] = b'WAVE'
        # Subchunk1 ID
        header[12:16] = b'fmt '
        # Subchunk1 size
        header[16:20] = (16).to_bytes(4, 'little')
        # Audio format (PCM)
        header[20:22] = (1).to_bytes(2, 'little')
        # Channels (Mono)
        header[22:24] = (1).to_bytes(2, 'little')
        # Sample rate (16kHz)
        header[24:28] = (16000).to_bytes(4, 'little')
        # Byte rate
        header[28:32] = (32000).to_bytes(4, 'little')
        # Block align
        header[32:34] = (2).to_bytes(2, 'little')
        # Bits per sample
        header[34:36] = (16).to_bytes(2, 'little')
        # Subchunk2 ID
        header[36:40] = b'data'
        # Subchunk2 size
        header[40:44] = len(data).to_bytes(4, 'little')
        
        return bytes(header) + data
    except Exception as e:
        print(f"❌ WAV doğrulama hatası: {str(e)}")
        return data

def get_sensor_summary():
    """Son sensör verilerini özetle"""
    try:
        with open(SENSOR_LOG, "r") as f:
            lines = f.readlines()
            if not lines:
                return ""
            
            # Son 10 veriyi al
            recent_data = [json.loads(line) for line in lines[-10:]]
            
            # Ortalama değerleri hesapla
            avg_temp = sum(d["sicaklik"] for d in recent_data) / len(recent_data)
            avg_hum = sum(d["nem"] for d in recent_data) / len(recent_data)
            avg_pres = sum(d["basinc"] for d in recent_data) / len(recent_data)
            
            # Son hareket durumu
            last_motion = recent_data[-1]["hareket"]
            
            # Özet metin
            summary = f"Son durum: {last_motion}. "
            summary += f"Son 10 ölçümün ortalaması: "
            summary += f"Sıcaklık {avg_temp:.1f}°C, "
            summary += f"Nem %{avg_hum:.1f}, "
            summary += f"Basınç {avg_pres:.1f}hPa."
            
            return summary
    except Exception as e:
        print(f"❌ Sensör özeti hatası: {str(e)}")
        return ""

@app.route("/upload", methods=["POST"])
def upload():
    try:
        print("\n=== YENİ İSTEK BAŞLADI ===")
        
        # Disk alanı kontrolü
        if not check_disk_space():
            return make_response("Disk alanı yetersiz", 507)
            
        print("📥 Ses verisi alınıyor...")
        print(f"Content-Type: {request.content_type}")
        print(f"Request boyutu: {request.content_length} bytes")
        print(f"Headers: {dict(request.headers)}")
        
        # İstek başlıklarını kontrol et
        is_first_chunk = request.headers.get('X-First-Chunk', 'false').lower() == 'true'
        is_last_chunk = request.headers.get('X-Last-Chunk', 'false').lower() == 'true'
        session_id = request.headers.get('X-Session-ID')
        
        print(f"First Chunk: {is_first_chunk}")
        print(f"Last Chunk: {is_last_chunk}")
        print(f"Session ID: {session_id}")
        
        if not session_id:
            print("❌ Session ID eksik!")
            return make_response("Session ID gerekli", 400)
        
        # Oturum dosyası yolu
        session_file = os.path.join(UPLOAD_FOLDER, f"{session_id}.wav.tmp")
        
        try:
            # İlk chunk için yeni dosya oluştur
            if is_first_chunk:
                print(f"🆕 Yeni kayıt oturumu başlatıldı: {session_id}")
                mode = 'wb'
            else:
                # Devam eden oturum için dosyaya ekle
                if not os.path.exists(session_file):
                    print(f"❌ Oturum dosyası bulunamadı: {session_file}")
                    return make_response("Geçersiz oturum", 400)
                mode = 'ab'
            
            # Chunk'ı dosyaya yaz
            with open(session_file, mode) as f:
                f.write(request.data)
            print(f"📝 Chunk yazıldı. Dosya boyutu: {os.path.getsize(session_file)} bytes")
            
            # Son chunk değilse OK döndür
            if not is_last_chunk:
                return make_response("OK", 200)
            
            # Son chunk için ses işlemeyi başlat
            print("🔄 Son chunk alındı, ses işleme başlıyor...")
            
            # Geçici dosyayı kalıcı dosyaya taşı
            file_id = str(uuid.uuid4())
            wav_path = os.path.join(UPLOAD_FOLDER, f"{file_id}.wav")
            os.rename(session_file, wav_path)
            print(f"💾 WAV dosyası kaydedildi: {wav_path}")
            
            # Whisper API'ye gönder
            print("🎯 Whisper API'ye gönderiliyor...")
            headers = {"Authorization": f"Bearer {GROQ_API_KEY}"}
            with open(wav_path, "rb") as af:
                try:
                    asr_res = requests.post(
                        "https://api.groq.com/openai/v1/audio/transcriptions",
                        headers=headers,
                        files={"file": (os.path.basename(wav_path), af, "audio/wav")},
                        data={"model": WHISPER_MODEL}
                    )
                    asr_res.raise_for_status()
                    user_text = asr_res.json().get("text", "").strip()
                    print(f"🗣️ Kullanıcı sorusu: {user_text}")
                except Exception as e:
                    print(f"❌ Whisper API hatası: {str(e)}")
                    raise

            # LLM'den yanıt al
            print("🤖 LLM API'ye gönderiliyor...")
            try:
                # Sensör verilerini al
                sensor_summary = get_sensor_summary()
                
                # Kullanıcı sorusunu ve sensör verilerini birleştir
                prompt = f"Kullanıcı sorusu: {user_text}\n\nGüncel sensör verileri: {sensor_summary}\n\nLütfen hem kullanıcının sorusunu yanıtla, hem de sensör verilerini yorumla."
                
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
                print(f"💡 LLM yanıtı: {reply}")
            except Exception as e:
                print(f"❌ LLM API hatası: {str(e)}")
                raise

            # gTTS → MP3 → kesin PCM WAV
            print("🔊 Ses sentezleniyor...")
            mp3_path = os.path.join(UPLOAD_FOLDER, f"{file_id}.mp3")
            wav_reply_path = os.path.join(UPLOAD_FOLDER, f"{file_id}_reply.wav")
            
            try:
                gTTS(reply, lang="tr").save(mp3_path)
                print(f"MP3 kaydedildi: {mp3_path}")

                # pydub ile MP3'ü decode edip raw PCM veriye dönüştür
                audio = AudioSegment.from_mp3(mp3_path)
                audio = audio.set_frame_rate(16000).set_channels(1).set_sample_width(2)
                raw_pcm = audio.raw_data

                # wave modülü ile baştan oluşturulan header + PCM
                with wave.open(wav_reply_path, "wb") as wf:
                    wf.setnchannels(1)         # mono
                    wf.setsampwidth(2)         # 16 bit = 2 byte
                    wf.setframerate(16000)     # 16 kHz
                    wf.writeframes(raw_pcm)

                print(f"✅ Yanıt WAV kaydedildi: {wav_reply_path}")
                print(f"WAV boyutu: {os.path.getsize(wav_reply_path)} bytes")
                
                # MP3'yi sil
                os.remove(mp3_path)
                
                # Orijinal WAV'ı sil
                os.remove(wav_path)
            except Exception as e:
                print(f"❌ Ses sentezleme hatası: {str(e)}")
                raise

            # URL'i dön
            host = request.host_url.rstrip('/')
            url = f"{host}/audios/{os.path.basename(wav_reply_path)}"
            print(f"📤 Dönen URL: {url}")
            resp = make_response(url, 200)
            resp.headers["Content-Type"] = "text/plain"
            return resp
            
        except Exception as e:
            print(f"❌ İşlem hatası: {str(e)}")
            # Hata durumunda geçici dosyayı temizle
            if os.path.exists(session_file):
                os.remove(session_file)
            raise

    except Exception as e:
        print(f"❌ Genel hata: {str(e)}")
        import traceback
        print(f"Stack trace:\n{traceback.format_exc()}")
        return make_response(str(e), 500)

@app.route("/audios/<filename>")
def serve_audio(filename):
    return send_from_directory(UPLOAD_FOLDER, filename)

@app.route("/sensor", methods=["POST"])
def sensor_data():
    try:
        data = request.get_json()
        print("📊 Sensör verisi alındı:", data)
        
        # Sensör verilerini dosyaya kaydet
        with open(SENSOR_LOG, "a") as f:
            f.write(json.dumps(data) + "\n")
        
        return make_response("OK", 200)
    except Exception as e:
        print(f"❌ Sensör verisi kaydedilirken hata: {str(e)}")
        return make_response(str(e), 500)

if __name__ == "__main__":
    # Temizlik zamanlayıcısını başlat
    cleanup_thread = threading.Thread(target=cleanup_timer, daemon=True)
    cleanup_thread.start()
    
    app.run(host="0.0.0.0", port=5000)