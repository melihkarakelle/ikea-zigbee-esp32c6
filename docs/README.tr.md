# IKEA TRÅDFRI Zigbee kontrolcüsü (XIAO ESP32-C6)

[English](../README.md) · **Türkçe**

ESP32-C6 kendi Zigbee ağını kurar (coordinator) ve evdeki WiFi'ye bağlanıp bir web sayfası sunar. Sayfaya evdeki herhangi bir telefon ya da bilgisayardan **http://ikea.local** adresiyle girilir.

- Her oda bir Zigbee grubudur.
- **Lambalar** eşlenirken odanın grubuna eklenir.
- **Kumandalar** (yuvarlak 5 tuşlu E1810/E1524, kare STYRBAR) odanın grubuna *bind* edilir. Kumandaya basınca komut doğrudan lambalara gider. ESP kapalıyken de kumandalar çalışır.
- Web sayfasından odaları açıp kapatabilir, parlaklık ve renk sıcaklığını ayarlayabilir, yeni cihaz ekleyebilir ve oda adlarını değiştirebilirsin.

## İlk kurulum: WiFi

1. Kartı herhangi bir USB şarj aletine tak.
2. Telefonda WiFi listesini aç ve **IKEA-Zigbee-Kurulum** ağına bağlan. Bu ağın şifresi yok.
3. Kurulum sayfası kendiliğinden açılır. Açılmazsa tarayıcıda `http://192.168.4.1` adresine git.
4. **Tara**'ya bas, evdeki WiFi'yi seç, şifresini yaz ve **Kaydet ve yeniden başlat**'a bas.
5. Telefonu tekrar evdeki WiFi'ye bağla ve `http://ikea.local` adresini aç.
   `ikea.local` açılmazsa modemin cihaz listesinde "ikea" adlı cihazın IP adresini bul ve onu kullan.

WiFi'yi değiştirmek ya da sıfırlamak için kartın üstündeki **BOOT** tuşunu 5 saniye basılı tut. Kart kurulum moduna döner. Firmware'e gömülü WiFi (`main/wifi_secret.h`) bundan sonra kullanılmaz; kurulum sayfasından yeni WiFi kaydedilene kadar kurulum ağı açık kalır. Ev ağı yalnızca geçici olarak gidince kart kurulum moduna geçmez, bağlanmayı denemeye devam eder.

Not: ESP32-C6 yalnızca 2,4 GHz WiFi'ye bağlanabilir.

## Durum LED'i

Kartın üstündeki küçük sarı LED (GPIO15) durumu gösterir:

| LED | Anlamı |
|---|---|
| BOOT basılıyken sürekli yanar | Tuşa basıldı, 5 saniye sayılıyor |
| Hızlı yanıp söner (5 Hz) | 5 saniye doldu, ya da kart kurulum modunda (IKEA-Zigbee-Kurulum ağı açık) |
| Yanıp söner (2 Hz) | Kurulum ağına bir telefon bağlandı. Normal çalışmada: eşleme açık |
| Yavaş yanıp söner (0,5 Hz) | Açılış: ev WiFi'sine bağlanıyor ya da Zigbee başlıyor |
| 2 saniyede bir kısa çakar | Ev WiFi'sine bağlı, her şey çalışıyor |

BOOT 5 saniye basılı tutulunca LED hızlı yanıp sönmeye başlar. Tuş bırakılınca kart yeniden başlar ve kurulum modunda da hızlı yanıp söner.

## Oda ekleme ve eşleme

> **Dikkat:** Sıfırlanan lamba ve kumanda eski eşleşmesini kaybeder. Önce tek bir odada dene.
> Geri dönmek istersen IKEA'nın normal yöntemi geçerlidir: kumandayı lambaya yakın tutup eşleme tuşuna basılı tut.

1. Sayfada **Cihaz ekle** bölümünde odayı seç ya da "+ Yeni oda…" seçip bir ad yaz, sonra **Eşlemeyi başlat**'a bas. Eşleme 3 dakika açık kalır.
2. **Lambaları sıfırla:** Odadaki lambaların duvar anahtarını 6 kez kapatıp aç. Lamba yanıp sönerek sıfırlandığını gösterir. Her lamba "Katılan cihazlar" listesinde görünür.
3. **Kumandayı sıfırla:** Kumandayı lambaların yakınında tut ve arkasındaki küçük eşleme tuşuna 5 saniye içinde 4 kez bas. Listede "Kumanda odaya bağlandı" yazmalı.
4. Olaylarda "Lamba ... odanın grubunda" ve "Kumandanın ok tuşları ESP'ye bağlandı" satırlarını gördükten sonra **Bitir**'e bas.

"Kumanda bağlanamadı" yazarsa kumandanın bir tuşuna bas, ardından hemen **Cihazlar** bölümünde o kumandanın odasını tekrar seç.

## Gece modu

Her oda kartının altında **Gece modu** anahtarı ve **Düzenle** düğmesi var. Düzenle, Ayarlar'daki **Gece modu** bölümünde o odanın satırını açar. Her oda için ayrı ayrı seçilir:

- saat aralığı (varsayılan 00:00–04:00)
- ne kadar sonra kısılacağı (1, 2, 5, 10, 15, 30, 60 dk; varsayılan 5 dk)
- hangi seviyeye kısılacağı (%1, 2, 5, 10, 20, 30, 50; varsayılan %1)

Bu saatlerde bir odanın ışığı açık ve seçilen seviyeden parlaksa, seçilen süre sonunda 3 saniyede o seviyeye kısılır. Işık kapanmaz, kapalı ışık da açılmaz. Gece biri ışığı tekrar açarsa ya da parlaklığı artırırsa süre yeniden başlar. Saat aralığı başladığında açık unutulmuş ışıklar da seçilen süre sonunda kısılır.
- Saat internetten (NTP, `pool.ntp.org`) alınır. Saat dilimi sayfayı açan tarayıcıdan öğrenilir ve kaydedilir, varsayılan Europe/London. Saat alınamazsa gece modu çalışmaz, üstteki etikette "Saat bekleniyor" yazar.
- Lambaların durumu (açık/kapalı, parlaklık, renk) lambaların ESP'ye gönderdiği bildirimlerden okunur. Kumandayla yapılan değişiklikler de sayfada görünür.

## Web API

Sayfanın kullandığı uç noktalar. Hepsi aynı zamanda başka araçlardan (ör. Home Assistant, curl) da çağrılabilir.

| İstek | Ne yapar |
|---|---|
| `GET /api/state` | Ağ durumu, odalar, cihazlar ve eşleme durumu (JSON) |
| `GET /api/events?after=N` | N'den sonraki olaylar |
| `POST /api/cmd` `room=<n>&a=on\|off\|toggle\|dim\|temp&v=<değer>` | `room=0` tüm lambalar demek. `dim` için 0–100, `temp` için Kelvin (2200–4000) |
| `POST /api/pair` `room=<n>&s=180` ya da `stop=1` | Eşlemeyi açar ya da kapatır |
| `POST /api/device` `i=<#>&room=<n>` ya da `i=<#>&forget=1` | Cihazı başka odaya taşır ya da listeden siler |
| `POST /api/room` `id=<n>&name=<ad>` | Oda adını değiştirir |
| `POST /api/room` `id=<n>&icon=<0-8>` | Oda çizimi. 0 otomatik (oda adına göre), 1 salon, 2 mutfak, 3 yatak odası, 4 banyo, 5 çocuk odası, 6 çalışma, 7 hol, 8 diğer |
| `POST /api/night` `id=<n>&on=1&from=0&to=4&pct=1&delay=5` | Odanın gece modu: saatler 0–23, `pct` kısılacak seviye 1–100, `delay` dakika 1–120 (`pct`/`delay` verilmezse eskisi kalır) |
| `POST /api/tz` `tz=<POSIX TZ>` | Saat dilimi, ör. `GMT0BST,M3.5.0/1,M10.5.0` |
| `GET /api/scan`, `POST /api/wifi` `ssid=..&pass=..` | WiFi tarama ve kaydetme |

Örnek:

```bash
curl -X POST http://ikea.local/api/cmd -d "room=1&a=off"
```

Güvenlik: Sayfada şifre yok. Ev ağındaki herkes ışıkları kontrol edebilir. Sayfa internete açılmamalıdır.

## Derleme ve yükleme

Gerekli: ESP-IDF v5.4 (`~/esp/esp-idf`). İstersen ev WiFi'sini firmware'e gömebilirsin: `main/wifi_secret.example.h` dosyasını `main/wifi_secret.h` olarak kopyala ve doldur (git'e girmez). Kurulum sayfasından ya da konsoldan kaydedilen WiFi bunun önüne geçer. Dosya yoksa kart kurulum modunda açılır. Zigbee ve mDNS kütüphaneleri ilk derlemede component manager tarafından indirilir.

```bash
. ~/esp/esp-idf/export.sh
idf.py set-target esp32c6
idf.py -p /dev/cu.usbmodem21101 flash
```

USB seri konsol yedek olarak duruyor. Açmak için `./monitor.sh` çalıştır, `help` yazınca komutlar listelenir (`pair`, `on`, `off`, `dim`, `temp`, `list`, `assign`, `ip`, `wifi`, `netreset`). Konsoldan çıkmak için `Ctrl+]`.

## Dosyalar

- `main/zigbee.c`: Zigbee coordinator, oda grupları, kumanda bind işlemi
- `main/web.c`: WiFi, kurulum ağı (captive portal), mDNS, HTTP API, BOOT tuşu
- `main/web/index.html`: web arayüzü, Türkçe ve İngilizce (Ayarlar → Görünüm'de dil ve tema; seçim tarayıcıda saklanır). Renk paleti ve düzen önerileri **Codex**'ten geldi.
- `main/web/svg/`: oda çizimleri (oda kartında çizime basıp seçilir; "Otomatik" seçiliyse oda adına göre gelir: salon, mutfak, yatak, banyo, çocuk, ofis, hol, diğer), logo (lamba ve kumanda bir arada; ampul ve ışın çizimi bilerek kullanılmadı), boş durum çizimi ve simgeler. Hepsini **Codex** çizdi. Işığı açık odada lambanın `.lit` kısmı parlar. Bir SVG değişince `python3 main/web/embed_svg.py` çalıştır, dosyalar `index.html` içine gömülür.
- `main/console.c`: USB seri konsol
- `main/events.c`: sayfada gösterilen olay listesi
- `main/clock.c`: internetten saat (SNTP) ve saat dilimi

## Bilinen sınırlar

- Kumandanın sol/sağ ok tuşları IKEA'ya özel komut gönderir ve lamba bunu grup üzerinden anlamaz. Bu yüzden ok tuşları doğrudan ESP'ye bağlanır, ESP de odaya renk sıcaklığı komutu gönderir (sol daha sıcak, sağ daha soğuk: 2200 / 2700 / 4000 K). Ok tuşları yalnızca ESP açıkken çalışır. Bağlantı kopmuşsa **Cihazlar** bölümünde kumandanın **Oklar** düğmesine bas ve 30 saniye içinde kumandanın bir tuşuna bas.
- WiFi ve Zigbee aynı radyoyu paylaşır. Zigbee kanal 25'te çalışır, WiFi ile en az çakışan kanallardan biridir.
