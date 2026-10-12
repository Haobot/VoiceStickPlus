#include "hotword_context.h"

#include <algorithm>
#include <cctype>
#include <map>

namespace voicestick {

namespace {

bool ContainsCaseInsensitive(const std::string& text, const std::string& needle) {
    if (needle.empty() || text.size() < needle.size()) return false;
    auto to_lower = [](char ch) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    };
    for (std::size_t i = 0; i + needle.size() <= text.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (to_lower(text[i + j]) != to_lower(needle[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

} // namespace

HotwordContextClassifier::HotwordContextClassifier(std::vector<HotwordContextRule> rules)
    : rules_(std::move(rules)) {}

HotwordContextScore HotwordContextClassifier::Score(const std::string& text) const {
    HotwordContextScore result;
    if (text.empty()) return result;
    for (const auto& rule : rules_) {
        if (rule.category.empty() || rule.keywords.empty()) continue;
        int hits = 0;
        for (const auto& keyword : rule.keywords) {
            if (ContainsCaseInsensitive(text, keyword)) ++hits;
        }
        if (hits > 0) {
            result.scores[rule.category] += rule.weight * hits;
            result.hits[rule.category] += hits;
        }
    }
    return result;
}

std::optional<std::string> HotwordContextClassifier::DecideCategory(
    const std::string& text,
    const std::string& current_category,
    const std::vector<std::string>& current_words,
    std::int64_t now_s) {
    if (text.empty()) {
        pending_category_.clear();
        pending_count_ = 0;
        return std::nullopt;
    }
    // 当前分类热词命中保护：用户正在使用当前热词库（最终文本中出现当前分类的
    // 热词），是强停留信号，直接放弃本次决策。
    for (const auto& word : current_words) {
        if (!word.empty() && ContainsCaseInsensitive(text, word)) {
            pending_category_.clear();
            pending_count_ = 0;
            return std::nullopt;
        }
    }
    const auto scored = Score(text);
    if (scored.scores.empty()) {
        pending_category_.clear();
        pending_count_ = 0;
        return std::nullopt;
    }
    // 最高分候选（排除当前分类）。
    const std::string* best = nullptr;
    double best_score = 0.0;
    for (const auto& [category, score] : scored.scores) {
        if (category == current_category) continue;
        if (!best || score > best_score) {
            best = &category;
            best_score = score;
        }
    }
    const auto reset_and_reject = [this]() {
        pending_category_.clear();
        pending_count_ = 0;
        return std::nullopt;
    };
    if (best == nullptr) return reset_and_reject();
    // ① 得分阈值；② 最少命中词数；③ 滞后：超过当前分类得分 + 加成。
    const double current_score =
        (scored.scores.count(current_category) > 0 ? scored.scores.at(current_category) : 0.0) +
        kHotwordCtxHysteresisBonus;
    const int best_hits = scored.hits.count(*best) > 0 ? scored.hits.at(*best) : 0;
    if (best_score < kHotwordCtxScoreThreshold ||
        best_hits < kHotwordCtxMinHits ||
        best_score <= current_score) {
        return reset_and_reject();
    }
    // ④ 连续性：连续 N 次决策指向同一分类。
    if (pending_category_ == *best) {
        ++pending_count_;
    } else {
        pending_category_ = *best;
        pending_count_ = 1;
    }
    // ⑤ 冷却：两次自动切换的最小间隔（冷却期内保留连续计数，到期即切）。
    if (last_switch_ts_ > 0 && now_s - last_switch_ts_ < kHotwordCtxCooldownS) {
        return std::nullopt;
    }
    if (pending_count_ < kHotwordCtxConsecutive) return std::nullopt;
    // ⑥ 目标非当前分类（候选选取时已排除），执行切换。
    last_switch_ts_ = now_s;
    pending_category_.clear();
    pending_count_ = 0;
    return *best;
}

void HotwordContextClassifier::Reset() {
    last_switch_ts_ = 0;
    pending_category_.clear();
    pending_count_ = 0;
}

std::vector<HotwordContextRule> HotwordContextClassifier::DefaultRules() {
    std::vector<HotwordContextRule> rules;
    // 程序：编码/工程/系统类关键词（含常见语言与工具名，ASCII 大小写不敏感）。
    rules.push_back({"程序",
                     {"代码", "编程", "程序", "函数", "变量", "编译", "调试", "运行",
                      "部署", "接口", "算法", "数据结构", "报错", "异常", "bug",
                      "缺陷", "重构", "提交", "分支", "合并", "回滚", "服务器",
                      "数据库", "前端", "后端", "客户端", "脚本", "框架", "类",
                      "对象", "继承", "多态", "线程", "进程", "内存", "指针",
                      "数组", "字符串", "语法", "断点", "单元测试", "集成测试",
                      "版本", "开发", "程序员", "编码", "python", "java",
                      "javascript", "typescript", "golang", "rust", "html",
                      "css", "sql", "linux", "git", "github", "api", "sdk",
                      "http", "tcp", "json", "yaml", "docker", "软件工程"},
                     1.0});
    // AI：模型/训练/智能类关键词。
    rules.push_back({"AI",
                     {"人工智能", "大模型", "大语言模型", "神经网络", "机器学习",
                      "深度学习", "模型", "训练", "推理", "微调", "提示词",
                      "智能体", "算力", "显卡", "张量", "数据集", "标注",
                      "过拟合", "梯度", "参数量", "token", "gpt", "多模态",
                      "语音识别", "图像识别", "自然语言", "知识图谱", "强化学习",
                      "生成式", "智能"},
                     1.0});
    // 数学：运算/几何/统计类关键词。
    rules.push_back({"数学",
                     {"数学", "方程", "微积分", "导数", "积分", "概率", "统计",
                      "几何", "代数", "定理", "证明", "公式", "计算", "矩阵",
                      "向量", "函数", "数列", "不等式", "三角形", "面积", "体积",
                      "坐标", "斜率", "极限", "收敛", "级数", "线性", "拟合",
                      "期望值", "方差", "分数", "小数", "整数", "质数", "开方",
                      "平方", "立方", "算术"},
                     1.0});
    // 语文：文学/语言学习类关键词。
    rules.push_back({"语文",
                     {"语文", "作文", "阅读理解", "古诗", "诗词", "文言文", "散文",
                      "小说", "段落", "文章", "修辞", "比喻", "拟人", "排比",
                      "拼音", "生字", "词语", "成语", "谚语", "默写", "背诵",
                      "课文", "作者", "中心思想", "主旨", "标点", "笔画", "偏旁",
                      "部首", "造句", "范文", "日记", "题材", "散文诗"},
                     1.0});
    // ===== 扩展领域（方案 §4.1：4 → 20）=====
    // 英语：英语学习类关键词。
    rules.push_back({"英语",
                     {"英语", "英文", "单词", "音标", "时态", "听力", "口语",
                      "发音", "语感", "介词", "冠词", "从句", "语法填空",
                      "english", "grammar", "ielts", "toefl", "listening",
                      "speaking"},
                     1.0});
    // 历史：历史学科类关键词。
    rules.push_back({"历史",
                     {"历史", "朝代", "王朝", "古代史", "近代史", "起义", "变法",
                      "考古", "文物", "遗址", "文明", "帝国", "皇帝", "史记",
                      "史料", "编年", "辛亥革命", "鸦片战争", "文艺复兴"},
                     1.0});
    // 地理：自然/人文地理类关键词。
    rules.push_back({"地理",
                     {"地理", "气候", "地形", "高原", "平原", "盆地", "山脉",
                      "河流", "经度", "纬度", "经纬", "板块", "地震", "火山",
                      "季风", "洋流", "降雨", "测绘", "等高线", "季风气候"},
                     1.0});
    // 物理：力学/电磁/光学类关键词。
    rules.push_back({"物理",
                     {"物理", "力学", "光学", "电磁", "磁场", "电场", "加速度",
                      "波长", "折射", "反射", "电路", "电阻", "电压", "电流",
                      "牛顿", "相对论", "量子", "惯性", "摩擦力", "功率"},
                     1.0});
    // 化学：元素/反应/实验类关键词。
    rules.push_back({"化学",
                     {"化学", "元素", "分子", "原子", "离子", "氧化", "还原",
                      "酸碱", "溶液", "催化剂", "化学键", "有机物", "无机物",
                      "蒸馏", "结晶", "周期表", "化学方程式", "试管", "滴定"},
                     1.0});
    // 生物：细胞/遗传/生态类关键词。
    rules.push_back({"生物",
                     {"生物", "细胞", "基因", "蛋白质", "光合作用", "呼吸作用",
                      "生态系统", "物种", "进化", "遗传", "变异", "细菌", "病毒",
                      "染色体", "新陈代谢", "神经元", "dna", "酶"},
                     1.0});
    // 医学：诊疗/健康类关键词。
    rules.push_back({"医学",
                     {"医学", "医生", "患者", "症状", "诊断", "治疗", "药物",
                      "手术", "病因", "感染", "炎症", "免疫", "血压", "血糖",
                      "临床", "处方", "疫苗", "康复", "化验", "病历"},
                     1.0});
    // 法律：法规/诉讼类关键词。
    rules.push_back({"法律",
                     {"法律", "法规", "条款", "合同", "诉讼", "原告", "被告",
                      "判决", "律师", "法院", "证据", "犯罪", "刑法", "民法",
                      "仲裁", "知识产权", "侵权", "违约", "立法"},
                     1.0});
    // 金融：财经/投资类关键词。
    rules.push_back({"金融",
                     {"金融", "股票", "基金", "债券", "利率", "汇率", "投资",
                      "理财", "期货", "证券", "股息", "牛市", "熊市", "通胀",
                      "市值", "估值", "存款", "贷款", "k线"},
                     1.0});
    // 音乐：乐理/演奏类关键词。
    rules.push_back({"音乐",
                     {"音乐", "歌曲", "旋律", "节奏", "和声", "音阶", "和弦",
                      "乐谱", "演唱", "演奏", "乐器", "钢琴", "吉他", "小提琴",
                      "交响乐", "歌词", "作曲", "编曲", "音准", "节拍"},
                     1.0});
    // 美术：绘画/设计类关键词。
    rules.push_back({"美术",
                     {"美术", "绘画", "素描", "色彩", "油画", "水彩", "国画",
                      "版画", "雕塑", "构图", "透视", "画展", "颜料", "笔触",
                      "写生", "临摹", "画廊", "插画"},
                     1.0});
    // 体育：运动/赛事类关键词。
    rules.push_back({"体育",
                     {"体育", "篮球", "足球", "乒乓球", "羽毛球", "游泳", "跑步",
                      "马拉松", "健身", "运动员", "奥运会", "冠军", "比分",
                      "进球", "球场", "教练", "联赛", "季后赛"},
                     1.0});
    // 烹饪：饮食制作类关键词（只用多字词，避免"蒸/煮"等单字子串误命中）。
    rules.push_back({"烹饪",
                     {"烹饪", "做菜", "菜谱", "调味", "食材", "火候", "腌制",
                      "烘焙", "面团", "发酵", "佐料", "炒菜", "炖汤", "红烧",
                      "清蒸", "口感", "咸淡"},
                     1.0});
    // 游戏：游玩/电竞类关键词。
    rules.push_back({"游戏",
                     {"游戏", "玩家", "关卡", "副本", "装备", "血量", "攻击力",
                      "排位", "匹配", "战队", "电竞", "主播", "手柄", "网游",
                      "单机游戏", "主机游戏", "通关", "存档"},
                     1.0});
    // 汽车：用车/养护类关键词。
    rules.push_back({"汽车",
                     {"汽车", "发动机", "变速箱", "底盘", "轮胎", "刹车", "油门",
                      "方向盘", "仪表盘", "续航", "保养", "机油", "油耗", "排量",
                      "四驱", "新能源车", "驾照", "行驶"},
                     1.0});
    // 航空航天：飞行/航天器类关键词。
    rules.push_back({"航空航天",
                     {"航天", "航空", "飞机", "火箭", "卫星", "飞船", "空间站",
                      "宇航员", "轨道", "发射", "探测器", "升空", "返回舱",
                      "推进器", "机翼", "起降", "航班", "客机"},
                     1.0});
    return rules;
}

std::string TopHotwordDomain(const HotwordContextScore& score) {
    const std::string* best = nullptr;
    double best_score = 0.0;
    for (const auto& [category, value] : score.scores) {
        if (!best || value > best_score) {
            best = &category;
            best_score = value;
        }
    }
    return best != nullptr ? *best : std::string();
}

ContentDepth ClassifyContentDepth(const std::string& text,
                                  const HotwordContextScore& score) {
    // UTF-8 字符数 = 非续字节计数（0b10xxxxxx 为续字节）。
    int char_count = 0;
    for (const char ch : text) {
        if ((static_cast<unsigned char>(ch) & 0xc0) != 0x80) ++char_count;
    }
    int total_hits = 0;
    for (const auto& [_, hits] : score.hits) total_hits += hits;
    if (char_count <= kDepthLightMaxChars || total_hits == 0) {
        return ContentDepth::kLight;
    }
    if (char_count > kDepthProMinChars || total_hits >= kDepthProMinHits) {
        return ContentDepth::kProfessional;
    }
    return ContentDepth::kStandard;
}

std::string BuildDomainHint(const std::string& domain, ContentDepth depth) {
    if (depth == ContentDepth::kLight) return {};
    // 领域语境提示（初版草稿，按方案 §8 决议 4 需逐领域人工评审后定稿）。
    static const std::map<std::string, std::string> kHints = {
        {"程序", "文本属于软件工程语境：代码标识符、命令与文件名保持原样，技术术语用行业通行写法。"},
        {"AI", "文本属于人工智能领域语境：模型名、算法名与专业缩写保持原文写法。"},
        {"数学", "文本属于数学学科语境：公式记号、定理与符号名称按数学规范表述。"},
        {"语文", "文本属于语文学科语境：人名、篇名、诗句按规范写法保留，引用原文不改写。"},
        {"英语", "文本属于英语学习语境：单词拼写、音标与例句保持原文，不翻译。"},
        {"历史", "文本属于历史学科语境：朝代、人名、事件名按史学界通行写法。"},
        {"地理", "文本属于地理学科语境：地名、地形与气候术语按标准地理名称。"},
        {"物理", "文本属于物理学科语境：物理量、单位与定律名称按规范表述。"},
        {"化学", "文本属于化学学科语境：元素、化合物名称与化学式保持规范写法。"},
        {"生物", "文本属于生物学科语境：物种名、解剖与遗传术语按规范表述。"},
        {"医学", "文本属于医学语境：疾病、药物与检查名称用规范医学用语。"},
        {"法律", "文本属于法律语境：法条、当事人称谓与法律术语保持规范表述。"},
        {"金融", "文本属于金融财经语境：机构名、产品名与指标术语按行业通行写法。"},
        {"音乐", "文本属于音乐语境：曲目、乐理术语与乐器名保持规范写法。"},
        {"美术", "文本属于美术语境：流派、技法与作品名按艺术史通行表述。"},
        {"体育", "文本属于体育语境：队名、人名与赛事术语保持通行译名或原名。"},
        {"烹饪", "文本属于烹饪语境：菜名、食材与技法名称按通行写法保留。"},
        {"游戏", "文本属于游戏语境：游戏名、角色名与术语保持官方写法。"},
        {"汽车", "文本属于汽车语境：车型、部件与养护术语按行业通行写法。"},
        {"航空航天", "文本属于航空航天语境：飞行器型号与专业术语保持规范表述。"},
    };
    const auto it = kHints.find(domain);
    if (it == kHints.end()) return {};
    std::string hint = it->second;
    if (depth == ContentDepth::kProfessional) {
        hint += "专业术语与专有名词必须保持原文，不得改写或翻译。";
    }
    return hint;
}

} // namespace voicestick
