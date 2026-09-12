# 🚀 SkyFrame: Native AI Frame Generation for Steam Deck

**SkyFrame** — это полностью открытый, нативный генератор промежуточных кадров (Frame Generation) для **Steam Deck (SteamOS / Linux)** на базе технологий **Vulkan FastWarp** и легковесной нейросети **RIFE 4.6 FlowNet**.

В отличие от `lsfg-vk`, SkyFrame **не использует проприетарные Windows DLL** из платного приложения *Lossless Scaling*, не требует Wine/Proton-прослоек для своего запуска и работает нативно через драйвер Vulkan (Mesa RADV) в среде **Gamescope**.

---

## ⚡ Особенности и преимущества

* **Нативный Vulkan Layer (`libVkLayer_skyframe.so`):** Прямой перехват буфера кадров в `vkQueuePresentKHR` без задержек перехвата экрана.
* **Шейдер деформации Vulkan FastWarp:** Аппаратный сдвиг пикселей на GPU Steam Deck (AMD Van Gogh RDNA 2) в нативном разрешении экрана (1280×800) за **~1.2 мс**.
* **Защита интерфейса (HUD & Crosshair Protection):** Алгоритмический фильтр локальной дисперсии предотвращает двоение и размытие прицелов, текста и миникарт.
* **Интеграция с Decky Loader:** Управление в 1 клик прямо из быстрого меню Steam Deck (кнопка `...`).
* **Чистый открытый код:** Без закрытых библиотек, полная совместимость с обновлениями SteamOS.

---

## 📊 Профили производительности (Steam Deck 800p)

| Профиль | Разрешение RIFE | Время RIFE (RDNA 2) | Время FastWarp (800p) | Итого задержка | Рекомендуемые игры |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **🚀 Лёгкий (Lite)** | **180p (320×180)** | ~5.2 мс | ~1.2 мс | **~6.4 мс** | Тяжёлые 3D (Cyberpunk 2077, Elden Ring, Wukong) |
| **⚖️ Баланс (Balanced)** | **240p (426×240)** | ~8.1 мс | ~1.2 мс | **~9.3 мс** | Оптимально: RPG, инди, эмуляторы (Switch, PS3) |
| **💎 Качество (Quality)** | **360p (640×360)** | ~14.0 мс | ~1.3 мс | **~15.3 мс** | Нетребовательные игры, CPU-bound проекты |

---

## 📦 Быстрая установка на Steam Deck

### Вариант 1: Через архив `SkyFrame.zip` (Desktop Mode)
1. Скопируйте файл **`SkyFrame.zip`** на Steam Deck (через флешку, Google Drive или KDE Connect).
2. На Steam Deck перейдите в **Режим рабочего стола (Desktop Mode)**.
3. Откройте файловый менеджер Dolphin и перейдите в папку:
   ```
   /home/deck/homebrew/plugins/
   ```
   *(Если папки нет, убедитесь, что у вас установлен [Decky Loader](https://decky.xyz/))*.
4. Распакуйте архив `SkyFrame.zip` прямо в эту папку (внутри должна появиться папка `SkyFrame`).
5. Вернитесь в игровой режим (Return to Gaming Mode).

### Вариант 2: Установка через SSH с компьютера
Если на Steam Deck включен SSH:
```bash
scp SkyFrame.zip deck@steamdeck.local:/home/deck/
ssh deck@steamdeck.local "unzip -o /home/deck/SkyFrame.zip -d /home/deck/homebrew/plugins/ && sudo systemctl restart plugin_loader.service"
```

---

## 🎮 Как пользоваться в играх

1. Запустите любую игру на Steam Deck.
2. Нажмите кнопку с тремя точками **`...`** (быстрое меню QAM).
3. Перейдите во вкладку плагинов **Decky Loader** (значок вилки/штекера).
4. Найдите **SkyFrame**:
   * Включите тумблер **«Генерация кадров (2x)»**.
   * Выберите профиль: **Лёгкий** (для тяжёлых игр) или **Баланс** (рекомендуемый).
   * Опция **«Защита интерфейса (HUD)»** активна по умолчанию.
5. Наслаждайтесь удвоенной плавностью (30 → 60 FPS или 40 → 80 FPS)!

---

## 🛠 Структура исходного кода

```
D:\SkyFrame/
├── CMakeLists.txt                  # Сборка Vulkan Layer под Linux x86_64
├── layer/                          # Исходники C++ Vulkan Layer
│   ├── skyframe_layer.cpp          # Точки входа Vulkan (vkGetInstanceProcAddr, vkQueuePresentKHR)
│   ├── skyframe_layer.h
│   ├── frame_pacer.cpp             # Фрейм-пейсинг, тайминги и выдача кадров
│   ├── frame_pacer.h
│   ├── vulkan_warper.cpp           # FastWarp для игровых RGBA8/BGRA8 текстур
│   ├── vulkan_warper.h
│   ├── flow_ncnn.cpp               # Инференс RIFE 4.6 FlowNet на GPU
│   ├── flow_ncnn.h
│   └── vk_layer_skyframe.json.in   # Манифест Vulkan Implicit Layer
├── shaders/                        # Вычислительные шейдеры Vulkan GLSL
│   ├── warp_rgba.comp              # FastWarp шейдер с защитой статического HUD
│   ├── warp_blend.comp             # Смешивание кадров
│   └── downsample.comp             # Быстрый compute даунскейл для RIFE
├── models/                         # Веса нейросети RIFE 4.6
│   ├── flownet.param
│   └── flownet.bin
├── decky-plugin/                   # Плагин Decky Loader для SteamOS
│   ├── plugin.json                 # Метаданные плагина
│   ├── package.json
│   ├── tsconfig.json
│   ├── src/index.tsx               # React-интерфейс в Quick Access Menu
│   └── main.py                     # Python-бэкенд для управления слоем
├── scripts/                        # Скрипты автоматизации
│   ├── build_linux.sh              # Сборка C++ слоя под Linux
│   ├── package_decky_zip.py        # Упаковка в SkyFrame.zip
│   └── install_decky.sh            # Скрипт быстрой установки
└── SkyFrame.zip                    # Готовый пакет для установки на Steam Deck
```

---

## ⚙️ Сборка из исходников (для разработчиков)

Сборка Vulkan-слоя на Linux (или в Ubuntu WSL2):
```bash
sudo apt-get update && sudo apt-get install -y cmake g++ libvulkan-dev glslang-tools
./scripts/build_linux.sh
python3 scripts/package_decky_zip.py
```
