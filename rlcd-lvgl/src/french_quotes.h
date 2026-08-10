#pragma once

/* 法语短句（状态栏顶部轮换显示）：french.md 筛选 <33 字符的句子，
 * 100 句（含 30 句超长句保守改写）。每句显示 10 分钟，期间 50s 法语 + 10s 中文
 * 循环切换（整分钟对齐：:00 开始法语、:50 切中文、下 :00 换句重新法语）。
 * 仅工作日 08:00-18:00 刷新。 */
static const char * const kFrenchQuotes[] = {
    "Bonjour, comment allez-vous?",  /* 001 [28] */
    "Je vais très bien, et vous?",  /* 002 [27] */
    "Merci beaucoup pour votre aide.",  /* 003 [31] */
    "Excusez-moi, où sont les WC?",  /* 004 [28] */
    "Pouvez-vous m'aider?",  /* 005 [20] */
    "Je ne comprends pas.",  /* 006 [20] */
    "Parlez-vous un peu anglais?",  /* 007 [27] */
    "Je parle un petit peu français.",  /* 008 [31] */
    "Comment vous vous appelez?",  /* 009 [26] */
    "Je m'appelle Alex, enchanté.",  /* 010 [28] */
    "Combien coûte ce produit?",  /* 011 [25] */
    "L'addition, s'il vous plaît.",  /* 012 [28] */
    "Pouvez-vous répéter doucement?",  /* 013 [30] */
    "Où se trouve la gare?",  /* 014 [21] */
    "Je cherche un restaurant.",  /* 015 [25] */
    "Montrez-moi le chemin.",  /* 016 [22] */
    "Tournez à gauche au croisement.",  /* 017 [31] */
    "Continuez tout droit 5 minutes.",  /* 018 [31] */
    "Quel âge avez-vous?",  /* 019 [19] */
    "Je viens de Chine, et vous-même?",  /* 020 [32] */
    "Où habitez-vous actuellement?",  /* 021 [29] */
    "Quel temps magnifique!",  /* 022 [22] */
    "Il fait un peu froid ce matin.",  /* 023 [30] */
    "Quelle heure est-il exactement?",  /* 024 [31] */
    "Il est midi, allons manger.",  /* 025 [27] */
    "Bonne journée à vous!",  /* 026 [21] */
    "À tout à l'heure!",  /* 027 [17] */
    "Bonne chance pour votre examen!",  /* 028 [31] */
    "Prenez bien soin de vous.",  /* 029 [25] */
    "C'est une excellente idée!",  /* 030 [26] */
    "Je suis tout à fait d'accord.",  /* 031 [29] */
    "Ce n'est pas grave du tout.",  /* 032 [27] */
    "C'est vraiment magnifique ici!",  /* 033 [30] */
    "J'aime beaucoup cette musique.",  /* 034 [30] */
    "Qu'est-ce que vous faites ici?",  /* 035 [30] */
    "Je travaille comme développeur.",  /* 036 [31] */
    "J'étudie l'informatique.",  /* 037 [24] */
    "Qu'est-ce que vous pensez de ça?",  /* 038 [32] */
    "Pourquoi pas, bonne idée!",  /* 039 [25] */
    "Je ne suis pas encore tout prêt.",  /* 040 [32] */
    "Je confirme, c'est vrai.",  /* 041 [24] */
    "Je suis un peu fatigué.",  /* 042 [23] */
    "Est-ce que vous êtes prêt?",  /* 043 [26] */
    "Allons-y ensemble!",  /* 044 [18] */
    "Faites très attention à vous!",  /* 045 [29] */
    "Ne t'inquiète pas, tout va bien.",  /* 046 [32] */
    "Ce n'est pas difficile à faire.",  /* 047 [31] */
    "Est-ce possible?",  /* 048 [16] */
    "J'ai besoin de votre aide.",  /* 049 [26] */
    "Passez un bon week-end, à lundi!",  /* 050 [32] */
    "À la prochaine fois, au revoir!",  /* 051 [31] */
    "Tout est parfaitement clair.",  /* 052 [28] */
    "J'adore écouter cette chanson.",  /* 053 [30] */
    "On se voit plus tard ce soir.",  /* 054 [29] */
    "Avez-vous une table pour deux?",  /* 055 [30] */
    "Je voudrais un café.",  /* 056 [20] */
    "Ce plat est vraiment très bon!",  /* 057 [30] */
    "J'ai très faim, allons manger.",  /* 058 [30] */
    "J'ai soif, je voudrais de l'eau.",  /* 059 [32] */
    "Où se trouve l'arrêt de bus?",  /* 060 [28] */
    "Je cherche un hôtel.",  /* 061 [20] */
    "Où est le centre?",  /* 062 [17] */
    "C'est à côté du parc.",  /* 063 [21] */
    "Je vis à Paris depuis deux ans.",  /* 064 [31] */
    "Il va pleuvoir cet après-midi.",  /* 065 [30] */
    "Il fait très chaud aujourd'hui.",  /* 066 [31] */
    "À demain matin, bonne nuit!",  /* 067 [27] */
    "Bon courage pour votre travail!",  /* 068 [31] */
    "Toutes mes félicitations!",  /* 069 [25] */
    "Joyeux anniversaire, mon ami!",  /* 070 [29] */
    "Bon appétit à tous les deux!",  /* 071 [28] */
    "Santé et bonheur à la famille!",  /* 072 [30] */
    "C'est une bonne idée.",  /* 073 [21] */
    "Je ne suis pas d'accord.",  /* 074 [24] */
    "C'est tout simplement parfait!",  /* 075 [30] */
    "Je n'aime pas du tout ce plat.",  /* 076 [30] */
    "Qu'est-ce qui se passe ici?",  /* 077 [27] */
    "Ce n'est pas la même chose.",  /* 078 [27] */
    "Ravi de vous connaître.",  /* 079 [23] */
    "Pouvez-vous fermer la porte?",  /* 080 [28] */
    "Il faut faire attention ici.",  /* 081 [28] */
    "J'ai oublié mon bagage ici.",  /* 082 [27] */
    "Est-ce que vous avez du temps?",  /* 083 [30] */
    "Je suis désolé pour ce retard.",  /* 084 [30] */
    "C'est un plaisir de vous voir.",  /* 085 [30] */
    "Quelle est votre adresse e-mail?",  /* 086 [32] */
    "Pouvez-vous m'expliquer ce mot?",  /* 087 [31] */
    "Je vais prendre un verre d'eau.",  /* 088 [31] */
    "Où voulez-vous aller ce soir?",  /* 089 [29] */
    "C'est une histoire incroyable!",  /* 090 [30] */
    "Je n'ai pas encore fini ceci.",  /* 091 [29] */
    "Il fait beau pour se promener.",  /* 092 [30] */
    "Pouvez-vous baisser le son?",  /* 093 [27] */
    "C'est une décision importante.",  /* 094 [30] */
    "J'espère que vous allez bien.",  /* 095 [29] */
    "Merci pour votre gentillesse!",  /* 096 [29] */
    "Je cherche la station de métro.",  /* 097 [31] */
    "Est-ce que c'est loin d'ici?",  /* 098 [28] */
    "On peut y aller à pied.",  /* 099 [23] */
    "À bientôt et bonne journée!",  /* 100 [27] */
};

/* 中文翻译：与 kFrenchQuotes 一一对应（索引相同）。
 * 每句 ≤18 字，全部在 chinese_14 字体 CJK 覆盖范围（0x4E00-9FA5）内，
 * 标点仅用字体兜底范围内的常用中文标点。 */
static const char * const kFrenchTranslations[] = {
    "你好,您最近好吗?",       /* 001 */
    "我很好,您呢?",           /* 002 */
    "非常感谢您的帮助.",       /* 003 */
    "请问洗手间在哪里?",       /* 004 */
    "您能帮我一下吗?",         /* 005 */
    "我不明白.",               /* 006 */
    "您会说一点英语吗?",       /* 007 */
    "我会说一点法语.",         /* 008 */
    "您叫什么名字?",           /* 009 */
    "我叫阿历克斯,幸会.",     /* 010 */
    "这个产品多少钱?",         /* 011 */
    "请结账.",                 /* 012 */
    "您能慢慢重复一遍吗?",     /* 013 */
    "火车站在哪里?",           /* 014 */
    "我在找一家餐厅.",         /* 015 */
    "请给我指路.",             /* 016 */
    "在路口向左转.",           /* 017 */
    "直走五分钟.",             /* 018 */
    "您多大了?",               /* 019 */
    "我来自中国,您呢?",       /* 020 */
    "您现在住在哪里?",         /* 021 */
    "天气真美啊!",             /* 022 */
    "今天早上有点冷.",         /* 023 */
    "现在确切是几点?",         /* 024 */
    "中午了,去吃饭吧.",       /* 025 */
    "祝您今天愉快!",           /* 026 */
    "一会儿见!",               /* 027 */
    "祝您考试顺利!",           /* 028 */
    "请照顾好自己.",           /* 029 */
    "这是个绝妙的主意!",       /* 030 */
    "我完全同意.",             /* 031 */
    "这完全不要紧.",           /* 032 */
    "这里真是美极了!",         /* 033 */
    "我非常喜欢这首音乐.",     /* 034 */
    "您在这里做什么?",         /* 035 */
    "我是一名开发人员.",       /* 036 */
    "我在学计算机.",           /* 037 */
    "您觉得这个怎么样?",       /* 038 */
    "为什么不呢,好主意!",     /* 039 */
    "我还没完全准备好.",       /* 040 */
    "我确认,这是真的.",       /* 041 */
    "我有点累了.",             /* 042 */
    "您准备好了吗?",           /* 043 */
    "我们一起去吧!",           /* 044 */
    "请多保重!",               /* 045 */
    "别担心,一切顺利.",       /* 046 */
    "这做起来不难.",           /* 047 */
    "有可能吗?",               /* 048 */
    "我需要您的帮助.",         /* 049 */
    "周末愉快,周一见!",       /* 050 */
    "下次见,再见!",           /* 051 */
    "一切都非常清楚.",         /* 052 */
    "我爱听这首歌.",           /* 053 */
    "我们今晚晚点见.",         /* 054 */
    "有两人桌吗?",             /* 055 */
    "我想要一杯咖啡.",         /* 056 */
    "这道菜真好吃!",           /* 057 */
    "我饿了,去吃饭吧.",       /* 058 */
    "我渴了,想喝点水.",       /* 059 */
    "公交站在哪里?",           /* 060 */
    "我在找一家旅馆.",         /* 061 */
    "市中心在哪里?",           /* 062 */
    "就在公园旁边.",           /* 063 */
    "我在巴黎住了两年.",       /* 064 */
    "今天下午要下雨.",         /* 065 */
    "今天非常热.",             /* 066 */
    "明早见,晚安!",           /* 067 */
    "祝您工作顺利!",           /* 068 */
    "衷心的祝贺!",             /* 069 */
    "生日快乐,我的朋友!",     /* 070 */
    "祝二位用餐愉快!",         /* 071 */
    "祝全家健康幸福!",         /* 072 */
    "这是个好主意.",           /* 073 */
    "我不同意.",               /* 074 */
    "简直完美!",               /* 075 */
    "我一点都不喜欢这道菜.",   /* 076 */
    "这里发生了什么事?",       /* 077 */
    "这不是同一回事.",         /* 078 */
    "很高兴认识您.",           /* 079 */
    "您能把门关上吗?",         /* 080 */
    "在这里要小心.",           /* 081 */
    "我把行李忘在这里了.",     /* 082 */
    "您有时间吗?",             /* 083 */
    "抱歉我迟到了.",           /* 084 */
    "见到您很荣幸.",           /* 085 */
    "您的电子邮箱是什么?",     /* 086 */
    "您能解释一下这个词吗?",   /* 087 */
    "我要喝一杯水.",           /* 088 */
    "您今晚想去哪里?",         /* 089 */
    "这真是个惊人的故事!",     /* 090 */
    "我还没做完这个.",         /* 091 */
    "天气很适合散步.",         /* 092 */
    "您能把声音调低吗?",       /* 093 */
    "这是个重要的决定.",       /* 094 */
    "希望您一切都好.",         /* 095 */
    "谢谢您的善意!",           /* 096 */
    "我在找地铁站.",           /* 097 */
    "离这里远吗?",             /* 098 */
    "我们可以走路去.",         /* 099 */
    "再见,祝您愉快!",         /* 100 */
};

#define FRENCH_QUOTES_COUNT 100
