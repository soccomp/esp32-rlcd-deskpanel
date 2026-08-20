# -*- coding: utf-8 -*-
"""200 组法语对话（A/B 法语 + A/B 中文），供 gen_french_dialogues.py 生成内置词库。
格式：每项 (a_fr, b_fr, a_cn, b_cn)。
撇号统一用 ASCII "'"，法语引号用 « »，中文引号用 “ ”。"""

DIALOGUES = [
    # 001
    ("Bonjour ! Comment vous vous appelez ?", "Je m'appelle Thomas. Et vous ?",
     "你好！请问您叫什么名字？", "我叫托马斯。您呢？"),
    # 002
    ("Salut ! Ça va ?", "Ça va très bien, et toi ?",
     "嗨！最近好吗？", "挺好的，你呢？"),
    # 003
    ("Enchanté de vous rencontrer.", "Tout le plaisir est pour moi.",
     "很高兴认识您。", "这是我的荣幸。"),
    # 004
    ("Tu viens d'où ?", "Je viens de Chine, et toi ?",
     "你来自哪里？", "我来自中国，你呢？"),
    # 005
    ("Vous habitez où à Paris ?", "J'habite dans le 13e arrondissement.",
     "您住在巴黎哪里？", "我住在第13区。"),
    # 006
    ("Bonne journée !", "Merci, à toi aussi !",
     "祝你有美好的一天！", "谢谢，你也是！"),
    # 007
    ("À bientôt !", "Oui, à demain !",
     "一会儿见！", "好，明天见！"),
    # 008
    ("Comment s'est passée ta journée ?", "Ça s'est très bien passé, merci.",
     "你今天过得怎么样？", "过得很不错，谢谢。"),
    # 009
    ("Je te présente mon ami Marc.", "Salut Marc, ravi de te rencontrer.",
     "向你介绍我的朋友马克。", "嗨马克，很高兴认识你。"),
    # 010
    ("Bonsoir, vous avez passé un bon week-end ?", "Excellent, merci ! Et le vôtre ?",
     "晚上好，您周末过得愉快吗？", "太棒了，谢谢！您呢？"),
    # 011
    ("Ça fait longtemps !", "Oui, tellement content de te revoir !",
     "好久不见了！", "是啊，真高兴再次见到你！"),
    # 012
    ("Tu parles français ?", "Un petit peu, j'apprends encore.",
     "你会说法语吗？", "会一点点，我还在学习中。"),
    # 013
    ("Vous comprenez ce qu'il dit ?", "Oui, je comprends tout à fait.",
     "您听得懂他说什么吗？", "嗯，我完全听得懂。"),
    # 014
    ("Pouvez-vous parler plus lentement, s'il vous plaît ?", "Pas de problème, je répète.",
     "您能说慢一点吗，拜托了？", "没问题，我重说一遍。"),
    # 015
    ("Comment on dit ça en français ?", "On dit « ordinateur ».",
     "这个用法语怎么说？", "说“ordinateur”，电脑。"),
    # 016
    ("Bonne soirée !", "Merci, bonne soirée à vous aussi !",
     "祝你今晚愉快！", "谢谢，也祝您晚上愉快！"),
    # 017
    ("Au revoir, prévenez-moi quand vous arrivez.", "D'accord, pas de souci.",
     "再见，到了告诉我一声。", "好的，没问题。"),
    # 018
    ("Bon courage pour ton travail !", "Merci beaucoup, j'en ai besoin !",
     "工作加油！", "非常感谢，我很需要加油！"),
    # 019
    ("Tu es libre ce soir ?", "Désolé, je suis occupé ce soir.",
     "你今晚有空吗？", "抱歉，我今晚有事。"),
    # 020
    ("Quoi de neuf ?", "Rien de spécial, et toi ?",
     "近来有什么新鲜事？", "没什么特别的，你呢？"),
    # 021
    ("Quel temps fait-il aujourd'hui ?", "Il fait un soleil magnifique !",
     "今天天气怎么样？", "阳光特别好！"),
    # 022
    ("Il va pleuvoir cet après-midi ?", "Oui, n'oublie pas ton parapluie.",
     "今天下午会下雨吗？", "会的，别忘了带伞。"),
    # 023
    ("Il fait vraiment froid ce matin.", "Oui, je devrais mettre un manteau.",
     "今天早上真冷啊。", "是啊，我得穿件大衣了。"),
    # 024
    ("À quelle heure tu t'es réveillé ce matin ?", "Je me suis réveillé à 7 heures.",
     "你今天早上几点醒的？", "我早上7点醒的。"),
    # 025
    ("Tu as bien dormi ?", "Oui, j'ai dormi comme un bébé.",
     "你睡得好吗？", "很好，睡得很香。"),
    # 026
    ("Il fait quelle température dehors ?", "Il fait environ 18 degrés.",
     "外面大概多少度？", "大概18度左右。"),
    # 027
    ("Regarde, il neige !", "C'est tellement beau !",
     "看，下雪了！", "真美啊！"),
    # 028
    ("Tu prends un café le matin ?", "Toujours, je ne peux pas démarrer sans café.",
     "你早上喝咖啡吗？", "总喝，没咖啡醒不来。"),
    # 029
    ("Il y a du vent aujourd'hui.", "Oui, la porte s'est fermée brutalement.",
     "今天风很大。", "是的，门都被风吹关上了。"),
    # 030
    ("Tu vas prendre une douche maintenant ?", "Oui, juste après m'être étiré.",
     "你现在要洗澡吗？", "对，拉伸完就去。"),
    # 031
    ("Tu te couches tôt ce soir ?", "Oui, je suis vraiment fatigué.",
     "你今晚早点睡吗？", "嗯，我实在太累了。"),
    # 032
    ("C'est le printemps, les fleurs poussent !", "J'adore cette saison.",
     "春天到了，花儿都开了！", "我很喜欢这个季节。"),
    # 033
    ("C'est étouffant aujourd'hui, non ?", "Oui, allumons la climatisation.",
     "今天好闷热，对吧？", "对，我们把空调开起来吧。"),
    # 034
    ("À quelle heure tu pars au travail ?", "Je pars vers 8h30.",
     "你几点出发去上班？", "我大概8点半出发。"),
    # 035
    ("Tu as fait ton lit ?", "Oui, c'est fait.",
     "你整理床铺了吗？", "整理好了。"),
    # 036
    ("Le ciel est très gris aujourd'hui.", "Oui, je pense qu'il va y avoir un orage.",
     "今天天空好灰啊。", "是的，感觉要下雷阵雨了。"),
    # 037
    ("Tu prends le bus ou le métro le matin ?", "Je prends le métro, c'est plus rapide.",
     "你早上坐公交还是地铁？", "我坐地铁，比较快。"),
    # 038
    ("Le soleil se couche tôt en hiver.", "Oui, il fait nuit dès 17 heures.",
     "冬天太阳落山得真早。", "是啊，下午5点天就黑了。"),
    # 039
    ("As-tu aéré la chambre ?", "Oui, j'ai ouvert la fenêtre pendant 10 minutes.",
     "你给房间换气了吗？", "开窗通风了10分钟。"),
    # 040
    ("C'est une journée idéale pour une promenade !", "Tout à fait d'accord, allons au parc.",
     "今天真适合散步！", "完全赞同，我们去公园吧。"),
    # 041
    ("Qu'est-ce qu'on mange ce midi ?", "Une salade fraîche et des pâtes.",
     "我们中午吃什么？", "一份新鲜沙拉和意大利面。"),
    # 042
    ("Vous avez choisi votre plat ?", "Oui, je vais prendre le steak-frites, s'il vous plaît.",
     "您选好餐点了吗？", "来份牛排配薯条，谢谢。"),
    # 043
    ("Vous voulez un dessert ?", "Une tarte aux pommes pour moi, merci.",
     "您需要甜点吗？", "给我一份苹果派，谢谢。"),
    # 044
    ("Vous préférez de l'eau plate ou gazeuse ?", "De l'eau plate, s'il vous plaît.",
     "您想要纯净水还是气泡水？", "请给我纯净水。"),
    # 045
    ("Bon appétit !", "Merci, toi aussi !",
     "用餐愉快！", "谢谢，你也是！"),
    # 046
    ("L'addition, s'il vous plaît.", "Tout de suite, monsieur.",
     "请结账。", "好的先生，马上来。"),
    # 047
    ("Tu aimes la cuisine française ?", "J'adore le fromage et le pain chaud.",
     "你喜欢法国菜吗？", "我超爱奶酪和热面包。"),
    # 048
    ("Est-ce que c'est épicé ?", "Non, c'est très doux.",
     "这个辣吗？", "不辣，很温和。"),
    # 049
    ("Tu prends du sucre dans ton thé ?", "Non merci, je le bois nature.",
     "你茶里加糖吗？", "不加了，谢谢，我喝原味的。"),
    # 050
    ("Qu'est-ce que tu me conseilles comme plat ?", "Le canard confit est excellent ici.",
     "你推荐什么菜？", "这里的油封鸭非常好吃。"),
    # 051
    ("Je suis végétarien.", "Pas de problème, nous avons des plats sans viande.",
     "我吃素。", "没问题，我们有无肉菜品。"),
    # 052
    ("Tu as faim ?", "Oui, j'ai une faim de loup !",
     "你饿了吗？", "饿极了，饿得能吃下半头牛！"),
    # 053
    ("Tu veux goûter à mon plat ?", "Oui, volontiers !",
     "你想尝尝我的菜吗？", "好啊，乐意之至！"),
    # 054
    ("C'est délicieux !", "Je suis ravi que ça te plaise.",
     "真好吃！", "很开心你喜欢。"),
    # 055
    ("On commande une pizza ?", "Bonne idée, une Margherita me va très bien.",
     "我们点个比萨吗？", "好，玛格丽特比萨就行。"),
    # 056
    ("Tu veux un verre de vin rouge ?", "Juste un petit verre, merci.",
     "你想喝一杯红酒吗？", "就一小杯吧，谢谢。"),
    # 057
    ("Où est le sel, s'il te plaît ?", "Il est juste à côté du poivre.",
     "请问盐在哪里？", "就在胡椒粉旁边。"),
    # 058
    ("Tu sais cuisiner ?", "Je sais faire quelques plats simples.",
     "你会做饭吗？", "我会做几道简单的小菜。"),
    # 059
    ("Qu'est-ce qu'on prend au petit-déjeuner ?", "Des croissants et du jus d'orange !",
     "早餐我们吃什么？", "羊角面包和橙汁！"),
    # 060
    ("Le service est compris ?", "Oui, le service est inclus dans la note.",
     "包含服务费了吗？", "是的，服务费已计入账单。"),
    # 061
    ("Combien ça coûte ?", "Ça fait 15 euros, s'il vous plaît.",
     "这个多少钱？", "一共15欧元，谢谢。"),
    # 062
    ("Je peux payer par carte ?", "Oui, à partir de 1 euro.",
     "可以刷卡吗？", "可以，满1欧即可。"),
    # 063
    ("Vous avez ce modèle en taille M ?", "Laissez-moi vérifier en réserve.",
     "这个款式有M码吗？", "让我去仓库帮您查一下。"),
    # 064
    ("Où sont les cabines d'essayage ?", "Tout au fond à gauche.",
     "试衣间在哪里？", "走到底左转。"),
    # 065
    ("Ça me va bien ?", "Ça te va à ravir !",
     "我穿这个好看吗？", "适合极了！"),
    # 066
    ("Est-ce qu'il y a une réduction ?", "Oui, c'est en solde à moins 30%.",
     "有折扣吗？", "有的，打七折。"),
    # 067
    ("Je peux avoir un ticket de caisse ?", "Voilà, bonne journée.",
     "能给我小票吗？", "给您，祝您过得愉快。"),
    # 068
    ("C'est un peu trop cher pour moi.", "Nous avons aussi des options plus abordables.",
     "对我来说有点太贵了。", "我们也有更实惠的选择。"),
    # 069
    ("Vous cherchez quelque chose en particulier ?", "Non merci, je regarde seulement.",
     "您在寻找什么特别的商品吗？", "不用了，我随便看看。"),
    # 070
    ("Je voudrais rendre cet article.", "Vous avez le ticket de caisse ?",
     "我想退掉这个商品。", "您带收据小票了吗？"),
    # 071
    ("Voulez-vous un sac ?", "Non merci, j'ai mon propre sac.",
     "您需要袋子吗？", "不用了，我自己带了。"),
    # 072
    ("Pouvez-vous emballer ça comme cadeau ?", "Oui, bien sûr.",
     "能帮我包装成礼物吗？", "好的，没问题。"),
    # 073
    ("Vous payez en espèces ou par carte ?", "En espèces, s'il vous plaît.",
     "您付现金还是刷卡？", "付现金，谢谢。"),
    # 074
    ("C'est de quelle couleur ?", "C'est bleu marine.",
     "这是什么颜色的？", "是藏青色的。"),
    # 075
    ("Est-ce sous garantie ?", "Oui, c'est garanti deux ans.",
     "这个有保修吗？", "有的，保修两年。"),
    # 076
    ("Ces chaussures sont très confortables.", "Et elles sont très élégantes aussi.",
     "这双鞋非常舒服。", "而且也很优雅。"),
    # 077
    ("Vos vendeurs sont très sympas ici.", "Merci, au plaisir de vous revoir !",
     "你们这里的店员都很热情。", "谢谢，期待再次光临！"),
    # 078
    ("Je vais prendre ce pantalon.", "Suivez-moi à la caisse.",
     "我要买这条裤子。", "请跟我到收银台。"),
    # 079
    ("C'est ouvert le dimanche ?", "Oui, de 10h à 18h.",
     "周日营业吗？", "营业，从10点到18点。"),
    # 080
    ("Il vous reste de la monnaie ?", "Oui, voici votre monnaie : 2 euros.",
     "找零找对了吗？", "找您的零钱：2欧元。"),
    # 081
    ("Excusez-moi, où est la gare ?", "C'est tout droit, puis à gauche.",
     "请问，火车站怎么走？", "直走，然后左转。"),
    # 082
    ("Quel métro faut-il prendre pour aller au Louvre ?", "Prenez la ligne 1.",
     "去卢浮宫该坐哪条地铁线？", "坐1号线。"),
    # 083
    ("Le bus passe tous les combien ?", "Toutes les 10 minutes environ.",
     "公交车多久一班？", "大概每10分钟一班。"),
    # 084
    ("Combien coûte un ticket de métro ?", "C'est 2,15 euros.",
     "地铁票一张多少钱？", "2.15欧。"),
    # 085
    ("Est-ce que ce train va à Lyon ?", "Oui, c'est un train direct.",
     "这趟火车去里昂吗？", "对的，这是直达车。"),
    # 086
    ("Où puis-je trouver un taxi ?", "Il y a une station de taxi juste devant la sortie.",
     "我在哪能打到出租车？", "出口对面就有出租车。"),
    # 087
    ("Vous pouvez m'indiquer le chemin sur la carte ?", "Bien sûr, nous sommes ici.",
     "能在地图上指下路吗？", "当然可以，我们现在在这里。"),
    # 088
    ("Est-ce que c'est loin d'ici ?", "Non, c'est à 5 minutes à pied.",
     "离这儿远吗？", "不远，走路5分钟。"),
    # 089
    ("Attention à la marche !", "Ah, merci de me prévenir !",
     "注意台阶！", "啊，谢谢提醒！"),
    # 090
    ("Le vol est à l'heure ?", "Oui, pas de retard annoncé.",
     "航班准点吗？", "准点，目前没有晚点通知。"),
    # 091
    ("Où est l'arrêt de bus le plus proche ?", "Juste au coin de la rue.",
     "最近的公交站在哪？", "就在街角拐弯处。"),
    # 092
    ("On y va en voiture ou à pied ?", "C'est tout près, allons-y à pied.",
     "我们开车去还是走路去？", "很近，走路去吧。"),
    # 093
    ("Il y a des embouteillages aujourd'hui.", "Oui, la circulation est terrible.",
     "今天堵车严重。", "是啊，交通状况太糟糕了。"),
    # 094
    ("Vous êtes descendu à quelle station ?", "Je suis descendu à Châtelet.",
     "您是在哪个站下车的？", "我是在Châtelet站下的。"),
    # 095
    ("Le train part de quel quai ?", "Il part du quai numéro 4.",
     "火车在哪个站台出发？", "4号站台。"),
    # 096
    ("Vous pouvez m'attendre un instant ?", "Pas de problème, je reste ici.",
     "您能等我片刻吗？", "没问题，我就在这等。"),
    # 097
    ("Je me suis perdu.", "N'ayez pas peur, où souhaitez-vous aller ?",
     "我迷路了。", "别担心，您想去哪里？"),
    # 098
    ("Faut-il composter le billet ?", "Oui, avant de monter dans le train.",
     "票需要打卡打孔吗？", "需要的，上车前要打卡。"),
    # 099
    ("Est-ce que je peux me garer ici ?", "Non, c'est une zone interdite.",
     "我可以停这儿吗？", "不行，这里是禁停区。"),
    # 100
    ("Bon voyage !", "Merci, à très bientôt !",
     "旅途愉快！", "谢谢，很快再见！"),
    # 101
    ("Qu'est-ce que tu fais pendant ton temps libre ?", "J'aime lire des livres et jouer de la musique.",
     "你空闲时间做什么？", "我喜欢看书和玩音乐。"),
    # 102
    ("Tu fais du sport ?", "Oui, je cours tous les dimanches matin.",
     "你做运动吗？", "做的，我每周日早上跑步。"),
    # 103
    ("Tu joues d'un instrument ?", "Oui, je joue de la guitare.",
     "你会乐器吗？", "会的，我弹吉他。"),
    # 104
    ("Tu as vu le dernier film de Spielberg ?", "Oui, je l'ai trouvé incroyable !",
     "看了斯皮尔伯格新片吗？", "看了，我觉得棒极了！"),
    # 105
    ("Tu aimes voyager ?", "J'adore ça, j'aimerais visiter le Japon.",
     "你喜欢旅游吗？", "太喜欢了，很想去日本。"),
    # 106
    ("Quel est ton genre de musique préféré ?", "J'écoute surtout du rock et du jazz.",
     "你最喜欢什么音乐风格？", "我主要听摇滚和爵士。"),
    # 107
    ("Tu veux aller au musée ce week-end ?", "Pourquoi pas ! Quelle exposition ?",
     "这周末想去博物馆吗？", "好啊！看什么展览？"),
    # 108
    ("Tu regardes souvent des séries ?", "Oui, sur Netflix presque chaque soir.",
     "你经常看剧吗？", "几乎每晚都在Netflix上看。"),
    # 109
    ("Tu aimes cuisiner pendant le week-end ?", "Oui, ça me détend beaucoup.",
     "你喜欢在周末做饭吗？", "喜欢，这能让我非常放松。"),
    # 110
    ("Quel est ton livre préféré ?", "Le Petit Prince, sans hésitation.",
     "你最喜欢的一本书是什么？", "毫不犹豫，《小王子》。"),
    # 111
    ("Tu sais nager ?", "Oui, j'ai appris quand j'étais enfant.",
     "你会游泳吗？", "会的，我小时候学的。"),
    # 112
    ("Tu vas souvent au concert ?", "Dès que mon groupe préféré passe en ville.",
     "你经常去看演唱会吗？", "喜欢的乐队来城就去。"),
    # 113
    ("Tu aimes la peinture ?", "Beaucoup, j'essaie de peindre le week-end.",
     "你喜欢绘画吗？", "很喜欢，我周末尝试画画。"),
    # 114
    ("Tu as des animaux de compagnie ?", "J'ai un chat qui s'appelle Mimi.",
     "你有宠物吗？", "我有一只叫Mimi的猫。"),
    # 115
    ("Tu aimes faire du vélo ?", "Oui, c'est parfait pour explorer la ville.",
     "你喜欢骑自行车吗？", "喜欢，骑车逛城很合适。"),
    # 116
    ("On fait une partie d'échecs ?", "D'accord, mais tu es plus fort que moi !",
     "下盘国际象棋吗？", "行啊，不过你可比我厉害！"),
    # 117
    ("Tu écoutes des podcasts ?", "Oui, pendant mes trajets en transport.",
     "你听播客吗？", "听，在搭乘交通工具时听。"),
    # 118
    ("Tu prends des photos ?", "Oui, j'adore la photographie de paysage.",
     "你拍照片吗？", "拍啊，我喜欢风景摄影。"),
    # 119
    ("Tu aimes camper en montagne ?", "Oui, c'est super pour se ressourcer.",
     "你喜欢在山上露营吗？", "喜欢，非常养神放松。"),
    # 120
    ("Quelle est ta passion principale ?", "L'apprentissage des langues étrangères !",
     "你最主要的兴趣爱好是什么？", "学习外语！"),
    # 121
    ("Qu'est-ce que tu étudies ?", "J'étudie l'informatique à l'université.",
     "你学什么专业？", "我在大学学计算机科学。"),
    # 122
    ("Tu travailles dans quel domaine ?", "Je travaille dans le marketing.",
     "你在哪个领域工作？", "我从事市场营销工作。"),
    # 123
    ("La réunion commence à quelle heure ?", "À 14 heures précises dans la salle A.",
     "会议几点开始？", "下午2点整，在A会议室。"),
    # 124
    ("As-tu fini ce rapport ?", "Pas encore, il me faut encore une heure.",
     "你这份报告写完了吗？", "还没有，我还需要一个小时。"),
    # 125
    ("Tu peux m'envoyer un e-mail ?", "Oui, je te l'envoie tout de suite.",
     "你能给我发封邮件吗？", "好的，我马上发给你。"),
    # 126
    ("Comment s'est passé ton examen ?", "Je pense avoir réussi !",
     "你考试考得怎么样？", "我觉得我考过了！"),
    # 127
    ("C'est difficile d'apprendre le français ?", "Au début oui, mais avec de la pratique ça va.",
     "学法语难吗？", "刚开始难，多练习就好了。"),
    # 128
    ("Tu travailles chez toi ou au bureau ?", "En télétravail deux jours par semaine.",
     "你在家办公还是去办公室？", "每周居家办公两天。"),
    # 129
    ("Tu as beaucoup de travail aujourd'hui ?", "Oui, mon agenda est bien chargé.",
     "你今天工作多吗？", "是的，日程排得很满。"),
    # 130
    ("Pouvez-vous me partager le document ?", "Bien sûr, je vous mets en copie.",
     "能把这个文件分享给我吗？", "当然可以，我抄送给您。"),
    # 131
    ("Quand sont tes vacances ?", "En août, pendant deux semaines.",
     "你什么时候休假？", "八月份，休两周。"),
    # 132
    ("Ton chef est exigeant ?", "Il est strict, mais très juste.",
     "你的老板要求严格吗？", "很严厉，但非常公正。"),
    # 133
    ("On fait une pause café ?", "Oui, bonne idée, ma tête va exploser !",
     "我们去喝杯咖啡休息一下？", "好主意，我脑袋都要炸了！"),
    # 134
    ("Tu as besoin d'aide pour ce projet ?", "Ce n'est pas de refus, merci !",
     "这个项目你需要帮忙吗？", "那我不客气了，谢谢！"),
    # 135
    ("La présentation était excellente !", "Merci, j'y ai passé beaucoup de temps.",
     "演示非常精彩！", "谢谢，我花了不少时间。"),
    # 136
    ("Tu as révisé pour le test ?", "Oui, j'ai révisé toute la nuit.",
     "你为测验复习了吗？", "复习了，我熬夜看了一整晚。"),
    # 137
    ("Où est l'imprimante ?", "Au deuxième étage, près du couloir.",
     "打印机在哪？", "法式三楼，走廊旁。"),
    # 138
    ("Tu as un stylo à me prêter ?", "Tiens, voilà un stylo bleu.",
     "你有笔能借我一下吗？", "给你，这是一支蓝色笔。"),
    # 139
    ("Le projet avance bien ?", "Oui, nous respectons le planning.",
     "项目推进得顺利吗？", "挺好的，我们严格按计划走。"),
    # 140
    ("Bon courage pour tes révisions !", "Merci, j'espère que ça va payer !",
     "复习加油啊！", "谢谢，希望功夫不负有心人！"),
    # 141
    ("Tu es disponible vendredi soir ?", "Oui, je n'ai rien de prévu.",
     "你周五晚上有空吗？", "有的，我没什么安排。"),
    # 142
    ("On se voit où ?", "Devant le café central à 19h.",
     "我们在哪里见？", "下午7点在中央咖啡馆门口。"),
    # 143
    ("Tu veux boire un coup après le travail ?", "Volontiers !",
     "下班后想去喝一杯吗？", "太乐意了！"),
    # 144
    ("Désolé, je suis en retard !", "Ce n'est pas grave, je viens d'arriver.",
     "抱歉，我迟到了！", "没事，我也刚到。"),
    # 145
    ("Merci pour l'invitation !", "Merci d'être venu !",
     "谢谢你的邀请！", "感谢你的到来！"),
    # 146
    ("Tu peux venir à mon anniversaire ?", "Avec grand plaisir !",
     "你能来参加我的生日派对吗？", "非常乐意！"),
    # 147
    ("Joyeux anniversaire !", "Merci beaucoup, c'est gentil !",
     "生日快乐！", "非常感谢，你太有心了！"),
    # 148
    ("On commande à emporter ou on sort ?", "Sortons, il fait super beau dehors.",
     "我们叫外卖还是出去吃？", "出去吃吧，外面天气好极了。"),
    # 149
    ("À quelle heure on se retrouve ?", "Disons 20h, ça te convient ?",
     "我们几点汇合？", "算8点吧，你方便吗？"),
    # 150
    ("Je peux amener un ami ?", "Oui, plus on est de fous, plus on rit !",
     "我能带个朋友一起吗？", "当然，人越多越热闹！"),
    # 151
    ("Tu as reçu mon message ?", "Ah oui, pardon, je n'ai pas répondu.",
     "你收到我的消息了吗？", "啊收到了，抱歉我忘了回。"),
    # 152
    ("Qu'est-ce qu'on fait ce week-end ?", "Si on allait au cinéma ?",
     "我们这周末做什么？", "要不去看电影吧？"),
    # 153
    ("C'était une super soirée !", "Oui, on s'est bien amusés !",
     "今晚玩得真开心！", "是的，我们过得很愉快！"),
    # 154
    ("Tu veux que je passe te chercher ?", "Ce serait génial, merci !",
     "需要我去顺路接你吗？", "那太棒了，谢谢！"),
    # 155
    ("On s'appelle demain ?", "Ça marche, appelle-moi dans l'après-midi.",
     "我们明天通电话？", "没问题，你下午打给我。"),
    # 156
    ("Tu me préviendras si tu changes d'avis ?", "Promis, je te tiens au courant.",
     "改主意了通知我一声？", "保证，随时保持沟通。"),
    # 157
    ("Tu connais du monde ici ?", "Juste l'hôte, et toi ?",
     "你认识这里的人吗？", "就认识主人，你呢？"),
    # 158
    ("C'est d'accord pour samedi ?", "C'est noté dans mon agenda.",
     "周六就这么定了？", "已记在日程表上了。"),
    # 159
    ("Passe un bon week-end !", "Merci, toi aussi, profite bien !",
     "周末愉快！", "谢谢，你也好好享受！"),
    # 160
    ("Rentre bien !", "Merci, à très vite !",
     "回去路上安全！", "谢谢，咱们很快再见！"),
    # 161
    ("Tu es sûr de toi ?", "Absolument certain !",
     "你确定吗？", "绝对确定！"),
    # 162
    ("Tu as l'air fatigué.", "Oui, j'ai mal dormi cette nuit.",
     "你看起来有点累。", "是的，我昨晚没睡好。"),
    # 163
    ("Je suis tellement content pour toi !", "Merci, ton soutien me touche.",
     "我太为你开心了！", "谢谢，你的支持让我很感动。"),
    # 164
    ("Qu'est-ce que tu en penses ?", "Je trouve que c'est une excellente idée.",
     "你觉得怎么样？", "我觉得这是个绝妙的主意。"),
    # 165
    ("Je ne suis pas d'accord avec toi.", "Explique-moi pourquoi.",
     "我不太赞同你的看法。", "告诉我为什么。"),
    # 166
    ("Je suis désolé pour ce qui s'est passé.", "Ce n'est pas grave, oublions ça.",
     "发生这种事我感到很抱歉。", "没事，忘掉它吧。"),
    # 167
    ("Pourquoi tu es en colère ?", "Parce que personne ne m'écoute.",
     "你为什么生气？", "因为根本没人听我说话。"),
    # 168
    ("C'est incroyable !", "Je n'en crois pas mes yeux !",
     "太难以置信了！", "我都不敢相信我的眼睛！"),
    # 169
    ("Tu es stressé ?", "Un peu, à cause de l'examen demain.",
     "你很紧张吗？", "有点，因为明天的考试。"),
    # 170
    ("Je suis soulagé !", "Tout s'est bien terminé, tant mieux.",
     "我终于松了一口气！", "一切顺利结束就好。"),
    # 171
    ("Tu me manques.", "Tu me manques aussi, vivement nos retrouvailles !",
     "我想你了。", "我也想你，真想快点重聚！"),
    # 172
    ("C'est dommage !", "Oui, c'est vraiment une opportunité manquée.",
     "真可惜！", "是啊，错失了好机会。"),
    # 173
    ("Tu as peur du noir ?", "Non, plus maintenant !",
     "你怕黑吗？", "不怕，现在不怕了！"),
    # 174
    ("Ne t'inquiète pas, tout va bien se passer.", "Merci pour tes mots rassurants.",
     "别担心，一切都会顺利的。", "谢谢你这番安慰的话。"),
    # 175
    ("À mon avis, c'est la meilleure solution.", "Oui, ça me paraît logique.",
     "依我看，这是最佳方案。", "嗯，听起来蛮合情合理。"),
    # 176
    ("Tu es déçu ?", "Un peu, j'espérais un meilleur résultat.",
     "你失望了吗？", "有点，本期待更好结果。"),
    # 177
    ("Je te fais confiance.", "Je ne te décevrai pas.",
     "我信任你。", "我不会让你失望的。"),
    # 178
    ("C'est ennuyeux, n'est-ce pas ?", "Oui, le temps passe vraiment lentement.",
     "真够无聊的，对吧？", "是啊，时间过得真慢。"),
    # 179
    ("Je suis fier de toi !", "Merci, ça me touche beaucoup.",
     "我为你感到骄傲！", "谢谢，这对我意义重大。"),
    # 180
    ("Tu es de bonne humeur aujourd'hui !", "Oui, le soleil me donne le sourire !",
     "你今天心情很好呀！", "是啊，阳光让人心情舒畅！"),
    # 181
    ("Au secours ! Aidez-moi !", "Qu'est-ce qui se passe ?",
     "救命！帮帮我！", "发生什么事了？"),
    # 182
    ("Appelez une ambulance, vite !", "D'accord, je compose le 15 immédiatement.",
     "快叫救护车！", "好的，我立刻打15号。"),
    # 183
    ("Où sont les toilettes, s'il vous plaît ?", "Au fond du couloir, à droite.",
     "请问洗手间在哪里？", "走廊尽头右转。"),
    # 184
    ("J'ai perdu mon passeport.", "Il faut aller déclarer la perte à la police.",
     "我的护照丢了。", "得赶紧去警察局报失。"),
    # 185
    ("Est-ce que quelqu'un parle anglais ici ?", "Oui, moi, je peux vous aider.",
     "这里有人会说英语吗？", "会的，我能帮您。"),
    # 186
    ("J'ai un problème avec mon ordinateur.", "Laisse-moi y jeter un coup d'œil.",
     "我的电脑出了点问题。", "让我来看一眼。"),
    # 187
    ("Où se trouve la pharmacie de garde ?", "Il y en a une ouverte sur la place.",
     "值班药房在哪里？", "广场上就有一家营业的。"),
    # 188
    ("J'ai de la fièvre et mal à la tête.", "Vous devriez aller voir un médecin.",
     "我发烧而且头疼。", "您应该去看医生。"),
    # 189
    ("Attention, c'est dangereux !", "Merci, je n'avais pas vu.",
     "小心，这很危险！", "谢谢，我刚才没注意到。"),
    # 190
    ("Pouvez-vous répéter, s'il vous plaît ?", "Oui, pas de problème, je disais que...",
     "请问您能重复一遍吗？", "好的，没问题，我刚才说……"),
    # 191
    ("J'ai oublié mes clés à l'intérieur.", "As-tu un double chez un voisin ?",
     "我把钥匙忘在屋里了。", "你在邻居那里有备用钥匙吗？"),
    # 192
    ("On m'a volé mon sac !", "Restez calme, décrivez-moi la personne.",
     "我的包被人偷了！", "冷静，描述一下那人的样子。"),
    # 193
    ("La ligne est occupée.", "Essaye de rappeler dans 5 minutes.",
     "电话占线中。", "试试5分钟后再打过去。"),
    # 194
    ("Est-ce que le Wi-Fi est gratuit ici ?", "Oui, quel est le mot de passe ?",
     "这里的Wi-Fi免费吗？", "免费，密码是什么？"),
    # 195
    ("Je me sens mal.", "Asseyez-vous et buvez de l'eau.",
     "我感觉不太舒服。", "您坐下喝口水吧。"),
    # 196
    ("Où est le commissariat le plus proche ?", "Prenez la deuxième rue à droite.",
     "最近的警察局在哪？", "走第二个路口右转。"),
    # 197
    ("Mon téléphone n'a plus de batterie.", "Tu peux utiliser le mien.",
     "我的手机没电了。", "你可以用我的。"),
    # 198
    ("Pouvez-vous garder mes affaires un instant ?", "Pas de souci, je veille dessus.",
     "您能帮我看一下东西吗？", "放心吧，我帮看着。"),
    # 199
    ("Il y a une erreur dans la facture.", "Ah désolé, je corrige ça tout de suite.",
     "账单上有错。", "啊抱歉，我立刻为您更正。"),
    # 200
    ("Merci infiniment pour votre aide !", "Je vous en prie, c'est tout naturel !",
     "非常感谢您的帮忙！", "不客气，这都是应该的！"),
]
