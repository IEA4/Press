#define DISP_TIMEOUT 5000     // таймаут для перехода с отображения неосновного параметра на основной

#define T_MAX_default 60      // температура отключения по умолчанию
#define P_MAX_default 5.0     // давление отключения ...
#define P_MIN_default 3.0     // давление включения ...

#define P_lim_off 2.0         // нижний предел давления отключения
#define P_LIM_off 10.0        // верхний ...   ...
#define P_lim_on 1.0          // нижний предел давления включения   (т.е. если давление будет ниже предел давления включения, компрессор включится)
#define P_LIM_on 9.0          // нижний предел давления включения   
#define dP 0.2                // шаг изменения давления выкл/вкл
#define Delta_P 0.1           // погрешность измерения давления (уровень шума)

#define T_LIM 95              // верхний предел рабочей температуры
#define T_lim 0               // нижний ...  ...  ... (значение условное, чтоб не накликать нереальные)
#define dT 5                  // шаг изменения ...

#define Yarkost 8             // яркость светодиодов (0.. 15) на дисплее (0 - мин значение, 15 - макс)

#define DS18B20_PIN A4        // пин с DS18B20 (для измерения температуры)

#define POT_PIN A0            // пин с потенциометром (имитация аналогового датчика давления)

#define LED_D4_PIN 10         // пин со светодиодом (для индикации включения реле (потом нужно будет сменить на реле))
#define BUZ_PIN 3             // пин с пищалкой (для индикации сброса к настройкам по умолчанию, подтверждения редактирования и выхода без подтверждения)

#define DIO_PIN 8             // пины 74HC595
#define CLK_PIN 7
#define LAT_PIN 4

#define BTN_UP_PIN A3         // пин кнопки вверх
#define BTN_DOWN_PIN A1       //   ...      вниз
#define BTN_MENU_PIN A2       //   ...

#define EB_NO_COUNTER         // отключить счётчик энкодера [VirtEncoder, Encoder, EncButton] (экономит 4 байта оперативки)
#define EB_NO_BUFFER          // отключить буферизацию энкодера (экономит 2 байта оперативки)

#define INIT_ADDR 1023        // номер ячейки для записи ключа первого запуска 
#define INIT_KEY 50           // ключ первого запуска. 0-254, на выбор

#include <EEPROM.h>             //  для работы с энергонезависимой памятью

#include <EncButton.h>          //  ... c кнопкой https://github.com/GyverLibs/EncButton
Button btnUP(BTN_UP_PIN);       // экзепляр кнопки "+"
Button btnDOWN(BTN_DOWN_PIN);   //   ...           "-"
Button btnMENU(BTN_MENU_PIN) ;  //   ...           "меню"

#include <GyverSegment.h>                     // https://github.com/GyverLibs/GyverSegment
Disp595_4 disp(DIO_PIN, CLK_PIN, LAT_PIN);

#include <GyverDS18.h>              // https://github.com/GyverLibs/GyverDS18
GyverDS18Single ds(DS18B20_PIN);    // пин с DS18B20

#include <GyverTimers.h>          // https://github.com/GyverLibs/GyverTimers

bool arr_f[5] = {};         // массив флагов для отоб-ия необ-ого параметра (текущих давления и тем-ры, макс-ных рабочей тем-ры и давления выкл., давления вкл)
uint32_t tmr_disp;          // переменная для хранения начала отсчёта DISP_TIMEOUT

int T, T_max;               // для измерения температуры и хранения максимальной рабочей температуры
float P_max, P_min, P;      // для хранения значения давления отключения и включения, измерения давления

volatile int cnt = 0;       // счётчик для выбора отображаемой величины (применяется и в прерывании по таймеру)

void setup() {
  Timer2.setPeriod(100);            // период тика дисплея (мкс)
  Timer2.enableISR(CHANNEL_A);      // вкл 2 таймер

  Timer1.setPeriod(300000);         // период мигания при редактировании ограничивающего параметра (мкс)

  disp.clear();                    // очистить дисплей
  disp.brightness(Yarkost);        // установить яркость (0.. 15)
  
  pinMode(LED_D4_PIN, OUTPUT);     // устанавливаем вывод как выход
  
  pinMode(BUZ_PIN ,  OUTPUT);         
  digitalWrite(BUZ_PIN, HIGH);        // Выключаем звук зуммера

  if(EEPROM.read(INIT_ADDR) != INIT_KEY) {    // первый запуск
    EEPROM.write(INIT_ADDR, INIT_KEY);        // записали ключ первого запуска
    EEPROM.put(0, T_MAX_default);             //   ...    температуру отключения по умолчанию
    EEPROM.put(2, P_MAX_default);             //   ...    давление отключения      ...
    EEPROM.put(6, P_MIN_default);             //   ...      ...    включения       ...
  }
  else {                                      // если запуск не первый из памяти берём:
    EEPROM.get(0, T_max);                     //  максимальную рабочую температуру,
    EEPROM.get(2, P_max);                     //  давление отключения,
    EEPROM.get(6, P_min);                     //  давление включения
  }
}

void loop() { 
  static int iter = 0;                          // итератор увеличения/уменьшения значения ограничивающего параметра
  static int iter_old = 0;                      //  ...     для хранения его предыдущего значения
  static bool f_ee_get = 0;                     // флажок прочтения из памяти
  static bool f_menu_clk = 0, f_menu_step = 0;  // флаги клика и наличия удержания кнопки меню
   
  btnUP.tick();           // опрос кнопок
  btnDOWN.tick();
  btnMENU.tick();

  static int T_old = 0;
  if (!ds.tick()) {               // автоматический опрос по таймеру. Вернёт DS18_READY (0) по готовности и чтению
    T = ds.getTempInt();
    if(T != T_old){               // разрешаем обновление отображаемой температуры только при его изменении
      T_old = T;
      arr_f[1] = 0;               // разрешаем отображение соответсвующего изменённого 1 параметра
    }
  }

  // этот "абзац" имитатор работы аналогового датчика давления на основе встроенного в модуль HW-262 потенциометра 
  static unsigned int val = 0, output_val = 0;    
  val = analogRead(POT_PIN);                      // cчитываем данные с потенциометра
  output_val = map(val, 0, 1023, 0, 110);         // преобразуем считываемые параметры на новый диапазон (только целые числа в диапазоне)
  P = output_val/10.0;                            // делаем дробной
  static float P_old = 0;                         // старое значение давления
  if(abs(P_old - P) > Delta_P){                   // разрешаем обновление только при его изменении выше шума
    P_old = P;
    arr_f[0] = 0;                                 // разрешаем отображение
  }

  if(T<T_max && P<P_min){              // включаем реле только в случае, если и текущая температура < максимальной рабочей и давление < давления включения      
    digitalWrite(LED_D4_PIN, LOW);
    arr_f[0] = 0;                      // разрешаем отображение
  }
  if(T>T_max || P>P_max){              // выключаем реле и в случае превышения макс. рабочей температуры и в случае превышения давления выше давления отключения 
    digitalWrite(LED_D4_PIN, HIGH);
    arr_f[0] = 0;
  }
  
  if(btnUP.click()) {         // при нажатии кнопки "+"
    if(f_menu_step == 0){     // если не было удержания кнопки меню 
      cnt++;                  // меняем значение счётчика для смены отображаемого параметра
      if(cnt == 5) cnt = 0;   // сбрасываем для отображения параметров по кругу
    }
    else iter++;              // в случае наличия удержания кнопки меню итерируем для смены ограничивающего параметра 
  }

  if(btnUP.step()) {                     // сброс настроек до заводских при удержании
    static uint32_t tmr_put = 0;         // для отсчёта таймаута в ...
    if(millis() - tmr_put > 10000){             // ... 10 сек на следующий сброс настроек до заводских 
      digitalWrite(BUZ_PIN, LOW);               // включаем звук пищалки
      EEPROM.put(0, T_MAX_default);             //   ставим температуру отключения по умолчанию
      EEPROM.put(2, P_MAX_default);             //   ...    давление отключения      ...
      EEPROM.put(6, P_MIN_default);             //   ...      ...    включения       ...
      T_max = T_MAX_default;                    // переписываем их на ограничивающие переменные
      P_max = P_MAX_default;
      P_min = P_MIN_default;
      delay(5);                                 // задержка для издания звука пищалкой
      tmr_put = millis();
      arr_f[cnt] = 0;                           // для отображения значений по умолчанию для соот-его параметра
      f_ee_get = 0;                             // тут зануляем, чтобы при следующем ред-нии брать именно тот параметр, который в памяти (если сбросили в момент ред-ия)
      f_menu_step = 0;                          // (если был сброс в момент редактирования)
      Timer1.disableISR(CHANNEL_A);             // (отключаем мигание, если был сброс в момент редактирования)
      digitalWrite(BUZ_PIN, HIGH);              // выключаем звук пищалки
    }
  }

  if(btnDOWN.click()) {       // действия по клику для "-" аналогично "+", только в обратную сторону
    if(f_menu_step == 0){     
      cnt--;
      if(cnt == -1) cnt = 4;
    }
    else iter--;
  }
    
  if(btnDOWN.step() && cnt != 0 && cnt != 1){         // наличие удержания кнопки "-" (нужно для выхода без сохранения) работает, только для редактируемых параметров
    digitalWrite(BUZ_PIN, LOW);                     // пищим
    arr_f[cnt] = 0;                                // для отображения значений по умолчанию для соот-его параметра
    f_ee_get = 0;
    f_menu_step = 0;                              
    Timer1.disableISR(CHANNEL_A);              // отключаем мигание    
    delay(8);                                  // задержка, чтоб знатно попищало
    digitalWrite(BUZ_PIN, HIGH);
  }

  if(btnMENU.click()){        // клик кнопкой "меню" (нужно для подтверждения изменения ограничивающих параметров)
    f_menu_clk = 1;           // отмечаем, что был клик
    iter = 0;                 // итераторы редактирвания параметров зануляем, чтоб в следующий раз начать заново отмечать измениние
    iter_old = 0;
    Timer1.disableISR(CHANNEL_A);  // отключаем мигание  
  }

  if(btnMENU.step() && (cnt!=0 && cnt!=1)){  // наличие удержания кнопки "меню" (нужно для входа в режим редактирования (!!!) ограничивающих параметров)
    f_menu_step = 1;          // разрешаем вход в режим редактирования
    f_menu_clk = 0;           // зануляем, чтобы исползовать для подтв-ия изменения: 1 при необ. подтв-ии изменения в меню
    tmr_disp = millis();
    Timer1.enableISR(CHANNEL_A); // включаем мигание сигнализатор редактирования 
  }

  switch(cnt){                // в зависимости от значения счётчика отображаем тот или иной параметр (P, T, P_max, P_min, T_max)
    case 0:                   // при отображении P ...
      if(arr_f[cnt] == 0){    // если давление не отображалось -- отображаем, а также в случае его измениния  
        P_disp(P, cnt);       // отображаем на дисплее
        kroneker(cnt);        // тут arr_f[0] = 1 становиться, а остальные нули
        iter = 0;             
        iter_old = 0;
      }
      break;
    case 1:                   // при отображении T ...
      if(arr_f[cnt] == 0){
        disp_T(T, cnt);
        kroneker(cnt);
        tmr_disp = millis();    // отмечаем время отсчёта перехода к отображению основного параметра в случае бездействия
      }
      break;
    case 2:                    // при отображении P_max ...
      if(f_menu_step == 0){    // если не в режиме редактирования
        if(arr_f[cnt] == 0){
          EEPROM.get(2, P_max);   //берём из памяти давление отключения
          P_disp(P_max, cnt);
          kroneker(cnt);
          tmr_disp = millis();
        }
      }
      else if(f_menu_step){          // если в режиме редактирования 
        static float val_P_max = 0;  // временная переменная для хранения значения давления отключения
        if(f_ee_get == 0) {
          f_ee_get = 1;               // отмечаем, что больше не нужно брать значение из памяти
          EEPROM.get(2, val_P_max);
          tmr_disp = millis();
        }
        if(iter > iter_old){          // если кликается "+"
          val_P_max += dP;            // увеличиваем на давление приращения
          iter_old = iter;
          val_P_max = constrain(val_P_max, P_lim_off, P_LIM_off);   // ограничиваем задающими параметрами, чтобы ненакликать нереальные значения
          P_disp(val_P_max, cnt); 
          tmr_disp = millis();
        } 
        else if (iter < iter_old){    // если кликается "-"
          val_P_max -= dP;
          iter_old = iter;
          val_P_max = constrain(val_P_max, P_lim_off, P_LIM_off);
          P_disp(val_P_max, cnt); 
          tmr_disp = millis();
        }        
        if(f_menu_clk) {                  // если при редактировании был клик кнопкой меню
          digitalWrite(BUZ_PIN, LOW);     // пищим
          f_menu_step = 0;                // кладём флаг редактирования
          f_ee_get = 0;                   // разрешаем брать из памяти значение
          arr_f[cnt] = 0;                 // для отображения сохраненного значения для соот-его параметра
          EEPROM.put(2, val_P_max);       // сохраняем
          static float val_P_min = 0;
          EEPROM.get(6, val_P_min);       // берём из памяти значение давления отключения
          if(val_P_min > (val_P_max - 1)){  // чтобы не было того, что дав вкл окажется > давления включения, делаем его на единицу меньше 
            P_min = val_P_max - 1;            // сразу изменяем и переменную включения (для стабильности срабатывания условия вкл/выкл) 
            EEPROM.put(6, P_min); 
          }
          delay(5);
          digitalWrite(BUZ_PIN, HIGH);
        }
      }
      break;
    case 3: 
      if(f_menu_step == 0){         // если не было удержания кнопки "меню"    
        if(arr_f[cnt] == 0){
          EEPROM.get(6, P_min);   
          P_disp(P_min, cnt);
          kroneker(cnt);
          tmr_disp = millis();          // включаем таймер перехода отображения основного параметра
        }
      }
      else if(f_menu_step){                 // в случае наличия удержания
        static float val_P_min = 0;
        if(f_ee_get == 0) {                 // берём данные из ЕЕPROM
          f_ee_get = 1;
          EEPROM.get(6, val_P_min);
          tmr_disp = millis();            
        }
        if(iter > iter_old){              // если кликается кнопка "+"
          val_P_min += dP;                // ... увеличиваем 
          iter_old = iter;
          val_P_min = constrain(val_P_min, P_lim_on, P_LIM_on);     // Ограничиваем диапазоном допустимых значений 
          P_disp(val_P_min, cnt);
          tmr_disp = millis();
        } 
        else if (iter < iter_old){        // если кликается кнопка "-"
          val_P_min -= dP;                // ... уменьшаем 
          iter_old = iter;
          val_P_min = constrain(val_P_min, P_lim_on, P_LIM_on); 
          P_disp(val_P_min, cnt);
          tmr_disp = millis();
        }        
        if(f_menu_clk) {                  // если при этом будет нажата кнопка 
          digitalWrite(BUZ_PIN, LOW);     
          f_menu_step = 0;
          f_ee_get = 0;
          arr_f[cnt] = 0;
          EEPROM.put(6, val_P_min);
          static float val_P_max = 0;
          EEPROM.get(2, val_P_max);
          if(val_P_min > (val_P_max - 1)){ 
            P_max = val_P_min+1;            // сразу изменяем и переменную выключения (для стабильности срабатывания условия вкл/выкл)
            EEPROM.put(2, P_max);
          }
          delay(5);
          digitalWrite(BUZ_PIN, HIGH);
        }
      }
      break;
    case 4:
      if(f_menu_step == 0){ 
        if(arr_f[cnt] == 0){
          EEPROM.get(0, T_max);
          disp_T(T_max, cnt);
          kroneker(cnt);
          tmr_disp = millis();
        }
      }
      else if(f_menu_step){
        static int val_T_max = 0;
        if(f_ee_get == 0) {
          f_ee_get = 1;
          EEPROM.get(0, val_T_max);
          tmr_disp = millis();
        }
        if(iter > iter_old){
          val_T_max += dT;
          iter_old = iter;
          val_T_max = constrain(val_T_max, T_lim, T_LIM); 
          disp_T(val_T_max, cnt); 
          tmr_disp = millis();
        } 
        else if (iter < iter_old){
          val_T_max -= dT;
          iter_old = iter;
          val_T_max = constrain(val_T_max, T_lim, T_LIM); 
          disp_T(val_T_max, cnt); 
          tmr_disp = millis();
        }        
        if(f_menu_clk) {
          digitalWrite(BUZ_PIN, LOW);
          f_menu_step = 0;
          f_ee_get = 0;
          arr_f[cnt] = 0;
          EEPROM.put(0, val_T_max);
          delay(5);
          digitalWrite(BUZ_PIN, HIGH);
        }
      }
      break;
  }
  if(cnt != 0  && millis()- tmr_disp > DISP_TIMEOUT){   // для неосновных параметров отображения  
    Timer1.disableISR(CHANNEL_A);
    f_menu_step = 0;
    cnt = 0;                // будем отображать давление
    f_ee_get = 0;
  }
}

void kroneker(int val_cnt){    // символ Кронекера (используется для однократного отображения необходимого параметра и включает разрешение отображения другого)
  for(int i=0; i < 5; i++){
    if(i == val_cnt) arr_f[i] = 1;
    else  arr_f[i] = 0;
  }
}

String reverse (String &S) {  // костыльная к библиотеке GyverSegment для правильного отображения значения на дисплее (переворачивает строку)
  String t = "";
  int k = S.length(); 
  for (int i=k-1; i>-1; i--){  
    t += S[i];
  }
  return t;                   // возвращает перевёрнутое значение
}

void P_disp(float &val_P, int n){   // функция отображения давления
  int i_num;
  if(val_P < 10 ) i_num = int(val_P * 10);  // при значениях >=10 дробная часть даёт  <10% погрешности ...
  else i_num = round(val_P);                  //    ... поэтому оставляем только округлённую целую часть 
  String s_num = String(i_num);               // преобразуем в строку  
  if(val_P < 10.0 && val_P >= 1){
    switch(n){
      case 0 ... 2:  s_num = reverse(s_num) + ".";  break; // z.B. val_P = 4.5   4.5 будет  "54", добавляем  ".", на отображение "54." | на дисплее будет отражённое 4.5 ((точка не отражается))
      case 3:        s_num = reverse(s_num) + "._";  break;
    }
  }
  else if(val_P >= 10.0){
    s_num = reverse(s_num);
    switch(n){
      case 3:  s_num += F("_"); break;          // z.B. 13  ->  "31_" | "_13"
    }
  }
  else if(val_P < 1){
    switch(n){
      case 0 ... 2: s_num += "0.";    break;            // z.B. 0.3  ->  "30." | "0.3"    (как бы case для 0, 1, 2, но задействуется только для 0 и 2)
      case 3:       s_num += "0._";   break;
    }
  }
  disp.clear();
  disp.setCursor(0);
  disp.print(s_num);
  if(n == 2){                 // для давления отключения ...
    disp.setCursor(2);        // на втором разряде ...
    disp.writeByte(0b00000001);   // ... зажигаем верхний горизонтальный светодиод
  }
  disp.setCursor(3);          // на третьем разряде ...
  disp.print("P");            // ... пишем "P"
  disp.update();              // обновляем дисплей
}

void disp_T(int &tmpr, int n){ // функция отображения температуры
  String n_str = String(tmpr);
  n_str = reverse(n_str);
  disp.clear();
  disp.setCursor(0);    // курсор в начало  (по разрядам слева направо: 3210)
  disp.print(n_str);
  if(n == 4){                     // для максимальной рабочей температуры ...
    disp.setCursor(2);
    disp.writeByte(0b00000001);   // ... зажигаем верхний горизонтальный светодиод
  }
  disp.setCursor(3);
  disp.print("T");
  disp.update();
}

ISR(TIMER2_A){          // в прерывании по 2 таймеру ... 
  disp.tick();          // ... вызываем тик дисплея
}

ISR(TIMER1_A){            // в прерывании по 1 таймеру мигаем точкой на самом левом разряде 
  static bool flag = 0;   // флажок смены состояния светодиода при мигании
  disp.setCursor(2);
  if(flag){                 // выключаем все светодиоды
    if(cnt == 2 || cnt == 4) disp.writeByte(0b00000000);  // для давлениия отключения  
    else if (cnt == 3)  disp.writeByte(0b00000000);       // для максимальной рабочей температуры
    flag = !flag;
  }
  else {
    if(cnt == 2 || cnt == 4) disp.writeByte(0b00000001);  
    else if (cnt == 3)  disp.writeByte(0b00001000);
    flag = !flag;
  }
  disp.update();
}
