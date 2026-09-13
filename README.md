# 🚀 SkyFrame: Native Frame Generation for Steam Deck (LSFG & DIS)

**SkyFrame** — это мощный генератор промежуточных кадров (Frame Generation) для **Steam Deck (SteamOS / Linux)** с **двухдвижковой архитектурой**:
1. ⭐ **LSFG Neural Engine**: нативное выполнение нейросети из купленной лицензионной копии *Lossless Scaling* из Steam с аппаратным ускорением FP16 на GPU AMD RDNA2 и поддержкой множителей **2x, 3x и 4x**!
2. 🚀 **SkyFrame DIS Native**: полностью автономный встроенный конвейер оптического потока DIS-Flow + TV-L1 с 3x3 векторной медианной очисткой и честным симметричным двунаправленным варпингом.

---

## ⚡ Особенности и преимущества

* **Нейросеть LSFG из Steam**: Прямое извлечение и запуск 24 скомпилированных SPIR-V шейдеров нейросети из вашего лицензионного файла `Lossless.dll`.
* **Аппаратное ускорение FP16**: На GPU Steam Deck (AMD Van Gogh RDNA 2) половинная точность выполняется за **~4–6 мс** на кадр при разрешении 720p/800p.
* **Множители 2x, 3x, 4x**: Легкое превращение 30 → 60 FPS, 30 → 90 FPS на OLED экране или 45 → 90 FPS.
* **Автономный резервный движок DIS**: Работает везде без необходимости устанавливать дополнительные файлы.
* **Полная поддержка 64-bit и 32-bit (Multilib)**: Скомпилированные слои для современных новинок и классических 32-битных игр.
* **Интеграция с Decky Loader**: Управление обоими движками, автоопределение `Lossless.dll` и переключение в 1 клик в быстром меню Steam Deck (кнопка `...`).

---

## 📋 Требование для LSFG Neural Engine в Steam

Чтобы использовать нейросетевой движок LSFG на Steam Deck:
1. Откройте библиотеку Steam на Steam Deck или ПК.
2. Нажмите правой кнопкой мыши (или шестеренку) на **Lossless Scaling** $\rightarrow$ **Свойства (Properties)**.
3. Перейдите во вкладку **Бета-версии (Betas)** $\rightarrow$ в выпадающем списке выберите ветку **`lsfg-vk`**.
4. Steam автоматически загрузит проверенную сборку `Lossless.dll` (LSFG 2.3 FP16).
5. В меню плагина SkyFrame загорится зелёный индикатор: `Lossless.dll обнаружен`.


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
