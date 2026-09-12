#ifndef NOMINMAX
#define NOMINMAX
#endif
// ノードグラフから評価用レイヤー列へのコンパイルを確かめる。
// GPU 評価の前段だけを対象にし、入力を外したときに古い結果を残さない規則を固定する。

#include "graph/NodeGraph.h"
#include "graph/CloudMapGenerator.h"
#include "graph/CloudShapeGenerator.h"
#include "renderer/CloudMotion.h"
#include "renderer/CloudSpatialIndex.h"
#include "renderer/CloudShapeCache.h"
#include "ui/AxisTranslationDrag.h"

#include "TestSupport.h"

#include <array>
#include <variant>
#include <cmath>

namespace {

using tg::graph::NodeGraph;
using tg::graph::NodeKind;
using tg::tests::Check;
using tg::tests::Section;

bool IsNeutralPlane(const tg::graph::CompiledGraph& compiled) {
    if (compiled.layers.size() != 1) {
        return false;
    }
    const tg::compositor::MaterialLayer& layer = compiled.layers.front();
    return layer.enabled && !tg::compositor::IsHeightOperationKind(layer.kind) &&
           layer.heightSource == tg::compositor::ValueSource::Constant &&
           layer.heightBase == tg::compositor::kHeightPivot;
}

bool StartsWithNeutralPlane(const tg::graph::CompiledGraph& compiled) {
    if (compiled.layers.empty()) {
        return false;
    }
    const tg::compositor::MaterialLayer& layer = compiled.layers.front();
    return layer.enabled && !tg::compositor::IsHeightOperationKind(layer.kind) &&
           layer.heightSource == tg::compositor::ValueSource::Constant &&
           layer.heightBase == tg::compositor::kHeightPivot;
}

}  // namespace

void RunNodeGraphTests() {
    {
        Section("Cloud Animation の接続と循環");
        NodeGraph graph;
        const auto shape=graph.CreateNode(NodeKind::CloudShapeGenerate);
        const auto noise=graph.CreateNode(NodeKind::CloudNoise);
        const auto animation=graph.CreateNode(NodeKind::CloudAnimation);
        const auto output=graph.CreateNode(NodeKind::CloudOutput);
        const auto link=[&](auto a,auto b) {
            return graph.CreateLink(graph.FindNode(a)->outputs[0].id,graph.FindNode(b)->inputs[0].id);
        };
        Check(link(shape,noise) && link(noise,animation) && link(animation,output),"Volume の後段へ接続できる");
        const auto compiled=graph.CompileCloud();
        Check(compiled.connected && compiled.cloud.animate && compiled.cloud.motionMode==3 &&
            compiled.sourceId==animation && !compiled.primitives.empty(),"形状を保持してループ設定を適用");
        auto& settings=std::get<tg::graph::CloudAnimationSettings>(graph.FindMutableNode(animation)->settings);
        settings.playing=false; settings.width=400; settings.speed=70;
        const auto paused=graph.CompileCloud();
        Check(!paused.cloud.animate && paused.animation.width==400 && paused.cloud.windSpeed==70 &&
            paused.cloud.width==compiled.cloud.width,"ループ範囲は元の形状ベイク範囲を変更しない");
        Check(paused.cloud.noiseSpeedRatio==1,"効果オフでは模様を維持");
        settings.evolveNoise=true; settings.noiseSpeedRatio=0.25f;
        Check(graph.CompileCloud().cloud.noiseSpeedRatio==0.25f,"効果オンで速度比を反映");
        settings.evolveNoise=false;
        Check(graph.CompileCloud().cloud.noiseSpeedRatio==1 && settings.noiseSpeedRatio==0.25f,"オフでも設定した速度比を保持");
        graph.DeleteNode(noise);
        Check(!graph.CompileCloud().connected,"入力切断で古い雲を残さない");
        using tg::renderer::CloudMotion;
        Check(CloudMotion::LoopOffset(1000,1000)==0 && CloudMotion::LoopOffset(-10,1000)==990 &&
            CloudMotion::LoopOffset(1000000025.0,1000)==25,"境界・逆方向・長時間の循環");
        CloudMotion motion;
        motion.Advance(1,true,100,1.57079632679f);
        const auto x=motion.x;
        motion.Advance(1,false,100,0);
        Check(motion.x==x && std::abs(x-100)<0.001,"停止位置を保持する");
        motion.Advance(1,true,100,1.57079632679f,0.25f);
        const auto drift=motion.driftX;
        motion.Advance(1,true,100,1.57079632679f,1.0f);
        Check(motion.driftX==drift,"効果オフは現在の模様を維持して形状だけ移動");
        motion.Advance(1,false,100,1.57079632679f,0.25f);
        Check(motion.driftX==drift,"一時停止は模様の変化も止める");
        motion.Reset();
        Check(motion.driftX==0 && motion.driftZ==0,"リセットで模様も初期状態へ戻る");
        Check(motion.x==0 && motion.z==0,"開始位置へ戻せる");
        tg::renderer::AtmosphereSettings a,b;
        b.cloudMotionMode=3; b.windOffsetX=500; b.loopWidth=2000;
        Check(tg::renderer::SameCloudShapeCache(a,b),"時間と循環範囲は形状再ベイクを起こさない");
    }

    {
        Section("雲形状ベイクの更新条件と格子範囲");
        tg::renderer::AtmosphereSettings a;
        a.primitiveCount=1;
        a.radiusX=a.radiusZ=500;
        a.cloudThickness=1000;
        const auto cube=tg::renderer::CloudShapeCacheSize(a);
        Check(cube==std::array<uint32_t,3>{192,192,192},"立方体は192点の格子、最大54MiB");
        auto b=a;
        b.seed++; b.cloudNoiseType++; b.primitiveDetail+=50; b.extinction*=2;
        b.elevation+=0.1f; b.samples=128; b.shapeCacheIndex=12;
        Check(tg::renderer::SameCloudShapeCache(a,b),"ノイズ・照明・密度・描画品質だけでは形状を焼き直さない");
        b.primitiveRevision++;
        Check(!tg::renderer::SameCloudShapeCache(a,b),"球の移動で形状を焼き直す");
        b=a; b.primitiveRevision+=2;
        Check(!tg::renderer::SameCloudShapeCache(a,b),"球の半径変更で形状を焼き直す");
        b=a; b.primitiveSmoothness+=1;
        Check(!tg::renderer::SameCloudShapeCache(a,b),"接合幅変更で形状を焼き直す");
        b=a; b.primitiveCount++;
        Check(!tg::renderer::SameCloudShapeCache(a,b),"球数変更で形状を焼き直す");
        b=a; b.radiusX*=2;
        Check(!tg::renderer::SameCloudShapeCache(a,b),"包囲範囲の変更で格子を更新する");
        b.radiusX=10000; b.radiusZ=1; b.cloudThickness=1000;
        const auto thin=tg::renderer::CloudShapeCacheSize(b);
        Check(thin[0]==512 && thin[1]>=8 && thin[1]<192 && thin[2]==8,
              "細長い範囲は短軸の格子数を減らし長軸の精度を上げる");
        bool bounded=true;
        for (int i=1;i<=100;++i) {
            b.radiusX=float(i*100); b.radiusZ=float((101-i)*37); b.cloudThickness=float(i*19);
            const auto grid=tg::renderer::CloudShapeCacheSize(b);
            bounded &= grid[0]*grid[1]*grid[2]<=192u*192u*192u;
            for (auto axis:grid) bounded &= axis>=8 && axis<=512;
        }
        Check(bounded,"縦横比によらずメモリ上限と各軸の格子数を守る");
    }

    {
        Section("手続き雲の形状構築");
        auto graph = NodeGraph::CreateDefault();
        const auto first=graph.CreateNode(NodeKind::CloudShapeGenerate);
        const auto second=graph.CreateNode(NodeKind::CloudShapeGenerate);
        const auto merge=graph.CreateNode(NodeKind::CloudMerge);
        const auto noise=graph.CreateNode(NodeKind::CloudNoise);
        const auto output=graph.CreateNode(NodeKind::CloudOutput);
        const auto link=[&](int from,int to,size_t index=0) {
            return graph.CreateLink(graph.FindNode(from)->outputs[0].id,graph.FindNode(to)->inputs[index].id);
        };
        const auto count=[&](int id) { return graph.CompileCloudShapes(id).primitives.size(); };
        Check(link(noise,output),"実験用の密度フィールドを既存雲出力へ接続できる");
        Check(!graph.CompileCloud().connected,"形状なしは雲を表示しない");
        Check(!link(noise,merge),"Volume を形状マージへ接続しない");
        std::get<tg::graph::CloudShapeGenerateSettings>(graph.FindMutableNode(second)->settings).centerX=3000;
        const size_t firstCount=count(first),secondCount=count(second);
        Check(firstCount>0 && secondCount>0,"Cloud Shape Generate は単独で球の集合を出す");
        Check(link(first,merge) && link(second,merge,1) && link(merge,noise),"形状構築チェーンを接続できる");
        const auto compiled=graph.CompileCloud();
        Check(compiled.connected && compiled.primitives.size()==firstCount+secondCount,"2つの積雲を同じフィールドにまとめる");
        Check(!compiled.layer && compiled.cloud.thickness>0,"地形と独立した立体の包囲箱を作る");
        const auto repeated=graph.CompileCloud();
        Check(repeated.primitives[0].centerX==compiled.primitives[0].centerX,"同じシードで同じ配置を再現する");
        Check(link(first,merge,2),"同じ形状を複数の入力に繋げる");
        Check(graph.CompileCloud().primitives.size()==firstCount+secondCount && !graph.CompileCloud().shapeOverflow,"共有された形状は一度だけ取り込む");
        Check(!link(merge,merge),"形状マージの循環を拒否する");
        graph.DeleteNode(first);
        Check(graph.CompileCloud().connected && graph.CompileCloud().primitives.size()==secondCount,"片方を削除しても残りの形状で雲を作る");
        graph.DeleteNode(second);
        Check(!graph.CompileCloud().connected,"形状削除後に古い形状を残さない");
    }

    {
        Section("手続き雲の包囲範囲と選択ガイド");
        NodeGraph graph;
        const auto shape=graph.CreateNode(NodeKind::CloudShapeGenerate);
        const auto noise=graph.CreateNode(NodeKind::CloudNoise);
        const auto output=graph.CreateNode(NodeKind::CloudOutput);
        const auto shapes=graph.CompileCloudShapes(shape);
        Check(!shapes.primitives.empty() && !graph.CompileCloud().connected,
            "未接続の形状もガイド用に評価できる");
        // 元形状の包囲範囲。滑らかさによる膨らみ（最大で滑らかさの 1/4）だけ余白が付く。
        float minX=1e9f,maxX=-1e9f,minY=1e9f,maxY=-1e9f,minZ=1e9f,maxZ=-1e9f;
        for (const auto& p:shapes.primitives) {
            minX=std::min(minX,p.centerX-p.radiusX); maxX=std::max(maxX,p.centerX+p.radiusX);
            minY=std::min(minY,p.centerY-p.radiusY); maxY=std::max(maxY,p.centerY+p.radiusY);
            minZ=std::min(minZ,p.centerZ-p.radiusZ); maxZ=std::max(maxZ,p.centerZ+p.radiusZ);
        }
        const float margin=shapes.smoothness*0.5f+0.01f;
        graph.CreateLink(graph.FindNode(shape)->outputs[0].id,graph.FindNode(noise)->inputs[0].id);
        graph.CreateLink(graph.FindNode(noise)->outputs[0].id,graph.FindNode(output)->inputs[0].id);
        auto& settings=std::get<tg::graph::CloudNoiseSettings>(graph.FindMutableNode(noise)->settings);
        Check(graph.CompileCloud().cloud.noiseType==2,"従来の手続き雲は低周波Perlinを維持する");
        settings.noiseType=1;
        Check(graph.CompileCloud().cloud.noiseType==0,"手続き雲のfBMを共通描画設定へ渡す");
        settings.noiseType=2;
        Check(graph.CompileCloud().cloud.noiseType==1,"手続き雲のPerlin-Worleyを共通描画設定へ渡す");
        settings.noiseType=0;
        settings.displacement=0;
        const auto exact=graph.CompileCloud().cloud;
        Check(exact.width>=maxX-minX-0.01f && exact.width<=maxX-minX+2*margin &&
              exact.thickness>=maxY-minY-0.01f && exact.thickness<=maxY-minY+2*margin &&
              exact.depth>=maxZ-minZ-0.01f && exact.depth<=maxZ-minZ+2*margin,
            "変位なしの包囲範囲は元形状の範囲に滑らかさの余白を足しただけ");
        settings.displacement=120;
        const auto bounds=graph.CompileCloud().cloud;
        Check(std::abs((bounds.thickness-exact.thickness)-240)<0.01f,
            "変位は高さに変位量の 2 倍だけ余白を加え、長軸用の余白を加えない");
        settings.detail=500; settings.feather=500;
        const auto inward=graph.CompileCloud().cloud;
        Check(inward.width==bounds.width && inward.thickness==bounds.thickness && inward.depth==bounds.depth,
            "内向きの削りと境界幅で包囲範囲を広げない");
        graph.DeleteNode(shape);
        Check(graph.CompileCloudShapes(shape).primitives.empty(),"削除した形状のガイドを残さない");
    }

    {
        Section("軸ドラッグの入力と取消");
        float x=10,y=20,z=30;
        float* values[]{&x,&y,&z};
        tg::ui::AxisTranslationDrag drag;
        drag.axis=1; drag.press={100,200}; drag.direction={0,-1}; drag.start=y; drag.metersPerPixel=2;
        Check(drag.Update({100,150},true,false,false,values) && y==120 && x==10 && z==30,"上向きY軸ドラッグはYだけを移動する");
        Check(!drag.Update({160,150},true,false,false,values) && y==120,"軸に垂直なドラッグで移動量が変わらない");
        Check(drag.Update({160,150},true,true,false,values) && y==20 && drag.axis==-1,"Escは押下前の移動量に戻す");
        drag.axis=0; drag.press={0,0}; drag.direction={1,0}; drag.start=x; drag.metersPerPixel=1;
        Check(drag.Update({40,0},true,false,false,values) && x==50,"ドラッグ開始位置から差分を加算する");
        Check(!drag.Update({40,0},false,false,false,values) && drag.axis==-1 && x==50,"ボタンを離すと位置を確定");
        Check(!drag.Update({80,0},false,false,false,values) && x==50,"リリース後のマウス移動では値を変えない");
        drag.axis=2; drag.start=z;
        Check(!drag.Update({80,0},true,false,true,values) && drag.axis==-1 && z==30,"Altの視点操作へ入力を譲る");
        drag.axis=0; drag.start=x;
        Check(drag.Update({20000,0},true,false,false,values) && x==10000,"ドラッグと数値入力は同じ移動範囲");
    }
    {
        Section("Cloud Shape Generateの単独積雲");
        NodeGraph graph;
        const auto node=graph.CreateNode(NodeKind::CloudShapeGenerate);
        const auto merge=graph.CreateNode(NodeKind::CloudMerge);
        auto& settings=std::get<tg::graph::CloudShapeGenerateSettings>(graph.FindMutableNode(node)->settings);
        settings.centerX=1000; settings.centerY=1500; settings.centerZ=-500;
        const auto base=graph.CompileCloudShapes(node);
        Check(base.connected && !base.primitives.empty() && !base.shapeOverflow,"既定設定で球の集合を生成");
        Check(std::abs(base.smoothness-180.0f)<0.01f,"滑らかさは球の半径（サイズ×間隔）×比率で決まる");
        settings.size=300;
        Check(std::abs(graph.CompileCloudShapes(node).smoothness-90.0f)<0.01f,"サイズを変えると滑らかさも比例して変わる");
        settings.size=600;
        bool bounded=true;
        const float planeY=1500-600*0.55f+0.3f*2*600*0.55f;
        for (const auto& p:base.primitives) {
            bounded &= std::abs(p.centerX-1000)<=600*1.6f && std::abs(p.centerZ+500)<=600*1.6f;
            bounded &= p.centerY-p.radiusY>=planeY-0.01f && p.radiusX==p.radiusY && p.radiusY==p.radiusZ;
            bounded &= p.originId==node;
        }
        Check(bounded,"球は中心付近に収まり、切り取り平面より下へ出ない");
        Check(graph.CompileCloudShapes(node).primitives.size()==base.primitives.size() &&
            graph.CompileCloudShapes(node).primitives[0].centerX==base.primitives[0].centerX,"同じ設定では同じ形状を再利用");
        settings.species=0;
        const auto humilis=graph.CompileCloudShapes(node);
        settings.species=2;
        const auto congestus=graph.CompileCloudShapes(node);
        float humilisTop=-1e9f,congestusTop=-1e9f;
        for (const auto& p:humilis.primitives) humilisTop=std::max(humilisTop,p.centerY+p.radiusY);
        for (const auto& p:congestus.primitives) congestusTop=std::max(congestusTop,p.centerY+p.radiusY);
        Check(congestusTop>humilisTop+300,"Congestusは塔でHumilisより高くなる");
        settings.species=1;
        settings.secondaryShapes=false;
        const auto plain=graph.CompileCloudShapes(node);
        settings.secondaryShapes=true; settings.iterations=2;
        const auto secondary=graph.CompileCloudShapes(node);
        Check(plain.primitives.size()<base.primitives.size() && secondary.primitives.size()>base.primitives.size(),"二次形状の繰り返しで球数が増える");
        settings.iterations=1;
        for (const auto* cloud:{&plain,&base,&congestus}) {
            bool overlapping=true;
            for (size_t i=0;i<cloud->primitives.size() && overlapping;++i) {
                const auto& a=cloud->primitives[i]; bool touched=cloud->primitives.size()==1;
                for (size_t j=0;j<cloud->primitives.size() && !touched;++j) {
                    if (i==j) continue;
                    const auto& b=cloud->primitives[j];
                    const float d=std::sqrt((a.centerX-b.centerX)*(a.centerX-b.centerX)+(a.centerY-b.centerY)*(a.centerY-b.centerY)+(a.centerZ-b.centerZ)*(a.centerZ-b.centerZ));
                    touched=d<a.radiusX+b.radiusX;
                }
                overlapping&=touched;
            }
            Check(overlapping,"すべての球が他の球と重なり、孤立した球を作らない");
        }
        settings.secondaryShapes=false;
        settings.seed++;
        Check(graph.CompileCloudShapes(node).primitives[0].centerX!=plain.primitives[0].centerX,"シード変更で配置を更新");
        settings.seed--;
        settings.secondaryShapes=true;
        settings.rotation=90;
        const auto rotated=graph.CompileCloudShapes(node);
        bool rotatedMatch=rotated.primitives.size()==base.primitives.size();
        for (size_t i=0;i<rotated.primitives.size() && rotatedMatch;++i) {
            const auto& a=base.primitives[i]; const auto& b=rotated.primitives[i];
            rotatedMatch &= std::abs((b.centerZ+500)-(a.centerX-1000))<0.01f && std::abs((b.centerX-1000)+(a.centerZ+500))<0.01f;
        }
        Check(rotatedMatch,"回転は上方向まわりの回転で球数を変えない");
        settings.rotation=0;
        Check(graph.CreateLink(graph.FindNode(node)->outputs[0].id,graph.FindNode(merge)->inputs[0].id) &&
            graph.CompileCloudShapes(merge).connected,"Cloud Mergeへ接続できる");
        settings.size=5000; settings.pointSeparation=0.05f; settings.length=settings.width=5;
        settings.secondaryShapes=true; settings.iterations=3;
        const auto excessive=tg::graph::GenerateCloudShape(settings,node);
        Check(excessive.shapeOverflow && !excessive.connected,"極端な設定は作業予算で停止する");
    }
    {
        Section("Cloud Map Generateの分布と成長");
        NodeGraph graph;
        const auto map=graph.CreateNode(NodeKind::CloudMapGenerate);
        const auto merge=graph.CreateNode(NodeKind::CloudMerge);
        auto& settings=std::get<tg::graph::CloudMapSettings>(graph.FindMutableNode(map)->settings);
        Check(settings.removeIsolated,"新規マップは孤立点を除外する");
        settings.removeIsolated=false;
        settings.width=settings.depth=1000; settings.pointCount=40; settings.connectionDistance=400;
        settings.centerX=250; settings.centerZ=-120; settings.bottomHeight=600; settings.columnsPerKm=0;
        const auto base=graph.CompileCloudShapes(map);
        Check(base.connected && base.mapGuide && base.mapGuide->points.size()==40,"指定数の点から雲底を生成");
        bool inBounds=true,edgesCorrect=true;
        size_t expectedEdges=0,isolated=0;
        for (size_t i=0;i<base.mapGuide->points.size();++i) {
            const auto a=base.mapGuide->points[i];
            inBounds &= a.x>=-250 && a.x<=750 && a.z>=-620 && a.z<=380;
            bool connected=false;
            for (size_t j=0;j<base.mapGuide->points.size();++j) {
                if (i==j) continue;
                const auto b=base.mapGuide->points[j];
                const bool close=std::hypot(b.x-a.x,b.z-a.z)<=400;
                connected |= close;
                if (j<=i) continue;
                size_t matches=0;
                for (const auto edge:base.mapGuide->edges) if (edge.a==i && edge.b==j) ++matches;
                edgesCorrect &= matches==(close?1u:0u);
                if (close) ++expectedEdges;
            }
            if (!connected) ++isolated;
        }
        Check(inBounds && edgesCorrect && base.mapGuide->edgeCount==expectedEdges,"セル検索が全ペア検索と一致し重複線を作らない");
        Check(base.primitives.size()==expectedEdges+isolated && base.mapGuide->columnCount==0,"成長密度0では接続線と孤立点の雲底のみ");
        bool bases=true;
        for (const auto& p:base.primitives) bases &= std::abs(p.centerY-p.radiusY-600)<0.001f &&
            p.radiusY==std::max(1.0f,std::min(100.0f,p.radiusX*0.5f)) && p.radiusX==p.radiusZ;
        Check(bases,"厚さを横幅に合わせても全雲底の底面高度を600mへ揃える");
        const auto cached=graph.CompileCloudShapes(map);
        Check(cached.mapGuide==base.mapGuide,"同じ設定では生成した分布を再利用");
        settings.removeIsolated=true;
        const auto filtered=graph.CompileCloudShapes(map);
        Check(filtered.primitives.size()==expectedEdges && filtered.mapGuide->edgeCount==base.mapGuide->edgeCount,
            "孤立点の除外は接続線とつながった雲底を変えない");
        Check(filtered.mapGuide->points[0].x==base.mapGuide->points[0].x,"孤立点除外の切替で散布位置を変えない");
        settings.columnsPerKm=10; settings.minGrowth=200; settings.maxGrowth=600;
        const auto grown=graph.CompileCloudShapes(map);
        bool columns=!grown.mapGuide->columns.empty();
        for (const auto& line:grown.mapGuide->columns) columns &= line.start.x==line.end.x && line.start.z==line.end.z &&
            line.end.y-line.start.y>0 && line.end.y-line.start.y<=200.01f;
        Check(columns && grown.primitives.size()>base.primitives.size(),"横幅の比率を優先し最小成長高さを下回る場合も制限");
        Check(grown.mapGuide->points[0].x==base.mapGuide->points[0].x,"成長設定を変えても元の散布点は維持");
        settings.seed++;
        Check(graph.CompileCloudShapes(map).mapGuide->points[0].x!=base.mapGuide->points[0].x,"シード変更で分布を更新");
        Check(graph.CreateLink(graph.FindNode(map)->outputs[0].id,graph.FindNode(merge)->inputs[0].id),"既存のCloud Mergeへ接続できる");
        Check(graph.CompileCloudShapes(merge).connected,"生成形状を後段の処理へ渡せる");
        settings.pointCount=1;
        const auto isolatedResult=graph.CompileCloudShapes(map);
        Check(isolatedResult.primitives.empty() && !isolatedResult.connected && isolatedResult.mapGuide->pointConnected[0]==0,"単独点は雲形状とガイドから除外できる");
        settings.removeIsolated=false;
        Check(graph.CompileCloudShapes(map).primitives.size()==1,"除外オフで孤立点の土台を復元する");
        settings.pointCount=0;
        Check(!graph.CompileCloudShapes(map).connected && graph.CompileCloudShapes(map).primitives.empty(),"0点で古い雲を消す");
        settings.width=settings.depth=100; settings.pointCount=10000; settings.connectionDistance=10000;
        settings.bottomThickness=2; settings.columnsPerKm=50; settings.minGrowth=settings.maxGrowth=5000;
        const auto excessive=tg::graph::GenerateCloudMap(settings,map);
        Check(excessive.shapeOverflow && !excessive.connected,"極端な生成は作業予算で停止し部分的な雲を表示しない");
    }
    {
        Section("Cloud Mapの横幅に比例した成長制限");
        tg::graph::CloudMapSettings settings;
        Check(settings.maxHeightRatio==0.5f,"高さ比率の既定値は0.5");
        settings.pointCount=2; settings.width=settings.depth=1000; settings.connectionDistance=10000;
        settings.columnsPerKm=50; settings.minGrowth=settings.maxGrowth=5000;
        const auto half=tg::graph::GenerateCloudMap(settings,1);
        const auto a=half.mapGuide->points[0],b=half.mapGuide->points[1];
        const float width=std::hypot(a.x-b.x,a.z-b.z);
        bool limited=!half.mapGuide->columns.empty();
        for (const auto& line:half.mapGuide->columns) limited &= std::abs(line.end.y-line.start.y-width*0.5f)<0.001f;
        Check(limited,"大きい成長高さの指定でも接続線の半分へ制限");
        settings.maxHeightRatio=1;
        const auto full=tg::graph::GenerateCloudMap(settings,1);
        bool samePositions=full.mapGuide->columns.size()==half.mapGuide->columns.size();
        for (size_t i=0;i<full.mapGuide->columns.size();++i) {
            const auto& line=full.mapGuide->columns[i];
            samePositions &= line.start.x==half.mapGuide->columns[i].start.x && line.start.z==half.mapGuide->columns[i].start.z;
            samePositions &= std::abs(line.end.y-line.start.y-width)<0.001f;
        }
        Check(samePositions,"比率変更は根元の位置とライン本数を維持");
        settings.maxHeightRatio=0;
        const auto flat=tg::graph::GenerateCloudMap(settings,1);
        Check(flat.connected && flat.primitives.size()==1 && flat.mapGuide->columnCount==0,"比率0は雲底のみを残す");
        settings.maxHeightRatio=1; settings.minGrowth=settings.maxGrowth=10;
        const auto shortGrowth=tg::graph::GenerateCloudMap(settings,1);
        bool staysShort=!shortGrowth.mapGuide->columns.empty();
        for (const auto& line:shortGrowth.mapGuide->columns) staysShort &= std::abs(line.end.y-line.start.y-10)<0.001f;
        Check(staysShort,"上限より低い成長高さを引き伸ばさない");
    }
    {
        Section("Cloud Mapの雲底厚さ制限");
        tg::graph::CloudMapSettings settings;
        Check(tg::graph::CloudMapHalfThickness(settings,20)==10,"直径40m・比率0.5の厚さは20m");
        Check(tg::graph::CloudMapHalfThickness(settings,500)==100,"大きい土台は指定厚さ200mを維持");
        settings.pointCount=1; settings.removeIsolated=false; settings.isolatedRadius=20; settings.bottomHeight=800;
        auto base=tg::graph::GenerateCloudMap(settings,1);
        Check(base.primitives[0].radiusY==10 && base.primitives[0].centerY==810,"孤立点にも比率と共通底面高度を適用");
        settings.maxThicknessRatio=1;
        base=tg::graph::GenerateCloudMap(settings,1);
        Check(base.primitives[0].radiusY==20 && base.primitives[0].centerY==820,"比率変更で厚さと中心を一緒に変更");
        settings.pointCount=2; settings.width=settings.depth=100; settings.connectionDistance=1000;
        settings.columnsPerKm=50; settings.minGrowth=settings.maxGrowth=100;
        base=tg::graph::GenerateCloudMap(settings,1);
        bool attached=!base.mapGuide->columns.empty();
        for (const auto& column:base.mapGuide->columns) attached &= column.start.y==base.primitives[0].centerY;
        Check(attached,"成長ラインの根元も各土台の中心高度に追従");
        settings.maxThicknessRatio=0.01f;
        Check(tg::graph::CloudMapHalfThickness(settings,1)==1,"極端に薄い土台は描画の最小厚さ2mに揃える");
    }
    {
        Section("Cloud Mergeの可変入力");
        NodeGraph graph;
        const auto merge=graph.CreateNode(NodeKind::CloudMerge);
        Check(graph.FindNode(merge)->inputs.size()==1,"新規マージは空き入力1個");
        std::array<tg::graph::GraphId,8> parents{},pins{};
        std::array<size_t,8> counts{};
        size_t total=0;
        for (size_t i=0;i<parents.size();++i) {
            parents[i]=graph.CreateNode(NodeKind::CloudShapeGenerate);
            // 位置をずらし、同じ設定でも別ノードとして数えられることを確かめる。
            std::get<tg::graph::CloudShapeGenerateSettings>(graph.FindMutableNode(parents[i])->settings).centerX=float(i)*2000;
            counts[i]=graph.CompileCloudShapes(parents[i]).primitives.size();
            total+=counts[i];
            pins[i]=graph.FindNode(merge)->inputs.back().id;
            Check(graph.CreateLink(graph.FindNode(parents[i])->outputs[0].id,pins[i]),"空きピンへ形状を接続");
            Check(graph.FindNode(merge)->inputs.size()==i+2,"接続ごとに空きピンが1個増える");
        }
        Check(total>0 && graph.CompileCloudShapes(merge).primitives.size()==total,"8入力すべてをマージする");
        const auto output=graph.FindNode(merge)->outputs[0].id;
        const auto spare=graph.FindNode(merge)->inputs.back().id;
        Check(!graph.CreateLink(output,spare) && graph.FindNode(merge)->inputs.size()==9,"拒否された循環でピンを増やさない");
        const auto linkId=graph.Links()[3].id;
        Check(graph.DeleteLink(linkId),"中間の接続を削除");
        Check(graph.FindNode(merge)->inputs.size()==8 && graph.FindPin(pins[4])!=nullptr,"空きを整理して後続の接続IDを維持");
        Check(graph.CompileCloudShapes(merge).primitives.size()==total-counts[3],"接続削除した形状だけを除く");
        const auto size=graph.FindNode(merge)->inputs.size();
        Check(graph.CreateLink(graph.FindNode(parents[0])->outputs[0].id,pins[1]),"接続済み入力を置換");
        Check(graph.FindNode(merge)->inputs.size()==size,"接続置換ではピン数を増やさない");
        const size_t replaced=total-counts[3]-counts[1];
        Check(graph.CompileCloudShapes(merge).primitives.size()==replaced,"置換で外れた形状を除き、重複した形状は一度だけ数える");
        NodeGraph restored;
        restored.Replace(graph.Nodes(),graph.Links());
        Check(restored.FindNode(merge)->inputs.size()==size && restored.FindNode(merge)->outputs[0].id==output,"復元後も入力数と出力IDを維持");
        Check(restored.CompileCloudShapes(merge).primitives.size()==replaced,"復元後も全接続と重複除外を維持");
        const auto extra=restored.CreateNode(NodeKind::CloudShapeGenerate);
        Check(restored.CreateLink(restored.FindNode(extra)->outputs[0].id,restored.FindNode(merge)->inputs.back().id),"復元後も入力を追加できる");
        Check(restored.FindNode(merge)->inputs.size()==size+1,"復元後の追加でも空き1個を維持");
        for (const auto parent:parents) graph.DeleteNode(parent);
        Check(graph.FindNode(merge)->inputs.size()==1 && !graph.CompileCloudShapes(merge).connected,"全入力元の削除で空き1個へ戻る");
    }
    {
        Section("雲トランスフォームの連結とマージ");
        NodeGraph graph;
        const auto parent=graph.CreateNode(NodeKind::CloudShapeGenerate);
        const size_t parentCount=graph.CompileCloudShapes(parent).primitives.size();
        const auto transform=graph.CreateNode(NodeKind::CloudTransform);
        const auto second=graph.CreateNode(NodeKind::CloudTransform);
        const auto merge=graph.CreateNode(NodeKind::CloudMerge);
        const auto link=[&](auto from,auto to,int pin=0) { return graph.CreateLink(graph.FindNode(from)->outputs[0].id,graph.FindNode(to)->inputs[pin].id); };
        Check(!graph.CompileCloudShapes(transform).connected,"未接続の変換は形状を出さない");
        Check(link(parent,transform) && link(transform,second),"Shapeの変換を連結できる");
        const auto original=graph.CompileCloudShapes(parent).primitives[0];
        auto& settings=std::get<tg::graph::CloudTransformSettings>(graph.FindMutableNode(transform)->settings);
        const auto identity=graph.CompileCloudShapes(transform).primitives[0];
        Check(identity.centerX==original.centerX && identity.radiusX==original.radiusX,"既定の移動量0で形状を維持");
        settings.translateX=123; settings.translateY=-45; settings.translateZ=67;
        const auto moved=graph.CompileCloudShapes(transform).primitives[0];
        Check(moved.centerX==original.centerX+123 && moved.centerY==original.centerY-45 && moved.centerZ==original.centerZ+67,"3軸の移動が反映される");
        Check(moved.radiusX==original.radiusX && moved.radiusY==original.radiusY && moved.radiusZ==original.radiusZ,"移動で球の半径を変えない");
        auto& next=std::get<tg::graph::CloudTransformSettings>(graph.FindMutableNode(second)->settings);
        next.translateX=-123; next.translateY=45; next.translateZ=-67;
        const auto restored=graph.CompileCloudShapes(second).primitives[0];
        Check(restored.centerX==original.centerX && restored.centerY==original.centerY && restored.centerZ==original.centerZ,"連結した逆移動で位置が戻る");
        Check(link(parent,merge) && link(transform,merge,1),"元形状と変換した形状をマージできる");
        Check(graph.CompileCloudShapes(merge).primitives.size()==2*parentCount,"同じ元形状でも移動した枝を重複除外しない");
        Check(link(transform,merge),"同じ変換を両入力へ分岐");
        Check(graph.CompileCloudShapes(merge).primitives.size()==parentCount,"同一変換の共有は重複を除外");
        Check(!link(second,transform),"変換を含む循環を拒否");
        graph.DeleteNode(parent);
        Check(!graph.CompileCloudShapes(second).connected,"入力削除で下流の変換も空になる");
    }
    {
        Section("手続き雲の雲底設定");
        NodeGraph graph;
        const auto parent=graph.CreateNode(NodeKind::CloudShapeGenerate);
        const auto noise=graph.CreateNode(NodeKind::CloudNoise);
        const auto output=graph.CreateNode(NodeKind::CloudOutput);
        graph.CreateLink(graph.FindNode(parent)->outputs[0].id,graph.FindNode(noise)->inputs[0].id);
        graph.CreateLink(graph.FindNode(noise)->outputs[0].id,graph.FindNode(output)->inputs[0].id);
        const auto before=graph.CompileCloud();
        auto& settings=std::get<tg::graph::CloudNoiseSettings>(graph.FindMutableNode(noise)->settings);
        Check(!before.cloud.flatBottom,"雲底を指定しない場合は従来形状");
        settings.flattenBottom=true; settings.bottomHeight=350; settings.bottomFeather=40;
        const auto after=graph.CompileCloud();
        Check(after.cloud.flatBottom && after.bottomHeight==350 && after.bottomFeather==40,"雲底の高さとぼかし幅を描画へ渡す");
        Check(before.cloud.centerY==after.cloud.centerY && before.cloud.thickness==after.cloud.thickness,
              "雲底の調整でベイク範囲を変えない");
        tg::renderer::AtmosphereSettings a,b;
        b.flatCloudBottom=1; b.proceduralBottomHeight=350; b.proceduralBottomFeather=40;
        Check(tg::renderer::SameCloudShapeCache(a,b),"雲底の変更では形状キャッシュを再生成しない");
    }
    {
        Section("雲の空間分割");
        constexpr uint32_t TestPrimitiveCount=4096;
        tg::renderer::CloudGeometry settings;
        settings.primitives.resize(TestPrimitiveCount);
        for (uint32_t i=0;i<TestPrimitiveCount;++i) {
            auto& p=settings.primitives[i];
            p.center[0]=float(i%16)*300; p.center[1]=float(i%7)*20; p.center[2]=float(i/16)*300;
            p.radius[0]=30+float(i%5); p.radius[1]=5+float(i%9); p.radius[2]=20;
        }
        tg::renderer::CloudGeometry few;
        few.primitives.assign(settings.primitives.begin(),settings.primitives.begin()+16);
        tg::renderer::BuildCloudSpatialIndex(few);
        Check(few.primitiveBvh.empty(),"16個以下は並べ替えず従来の評価を維持");
        tg::renderer::BuildCloudSpatialIndex(settings);
        Check(settings.primitiveBvh.size()<=TestPrimitiveCount && settings.primitiveBvh[0].escape==settings.primitiveBvh.size(),
            "4096形状の木を必要なサイズで構築する");
        std::array<int,TestPrimitiveCount> visits{};
        bool valid=true;
        for (uint32_t i=0;i<settings.primitiveBvh.size();++i) {
            const auto& node=settings.primitiveBvh[i];
            valid &= node.escape>i && node.escape<=settings.primitiveBvh.size() && node.count<=TG_CLOUD_BVH_LEAF_SIZE;
            for (uint32_t j=node.start;j<node.start+node.count;++j) { if(j<visits.size()) ++visits[j]; else valid=false; }
        }
        for (int count:visits) valid &= count==1;
        Check(valid,"各形状を1つの葉だけが参照し、飛び先が木の範囲内にある");
        const auto distanceAt=[](const auto& primitive,const float* position) {
            double k0=0,k1=0;
            for (int axis=0;axis<3;++axis) {
                const double r=primitive.radius[axis],o=position[axis]-primitive.center[axis];
                k0+=o*o/(r*r); k1+=o*o/(r*r*r*r);
            }
            k0=std::sqrt(k0);k1=std::sqrt(k1);
            return k1>1e-7 ? k0*(k0-1)/k1 : -double(std::min({primitive.radius[0],primitive.radius[1],primitive.radius[2]}));
        };
        bool conservative=true,accurate=true,boundedInflation=true;
        uint32_t tested=0;
        for (int sample=0;sample<64;++sample) {
            const float position[]={float(sample%8)*600-10,float(sample%5)*25,float(sample/8)*600-15};
            for (uint32_t i=0;i<settings.primitiveBvh.size();++i) {
                const auto& node=settings.primitiveBvh[i];
                const double lower=tg::renderer::CloudSpatialLowerBound(node,position);
                for (uint32_t j=i;j<node.escape;++j) {
                    const auto& leaf=settings.primitiveBvh[j];
                    for (uint32_t k=leaf.start;k<leaf.start+leaf.count;++k)
                        conservative &= lower<=distanceAt(settings.primitives[k],position)+0.001;
                }
            }
            const auto smoothUnion=[](double first,double second,double k) {
                if (k<=0) return first;
                const double h=std::max(k-(second-first),0.0)/k;
                return first-h*h*k*0.25;
            };
            for (double smoothness:{0.0,5.0,80.0}) {
                double minimum=1e30,secondMinimum=1e30;
                for (const auto& primitive:settings.primitives) {
                    const double d=distanceAt(primitive,position);
                    if (d<minimum) { secondMinimum=minimum; minimum=d; } else secondMinimum=std::min(secondMinimum,d);
                }
                const double reference=smoothUnion(minimum,secondMinimum,smoothness);
                double nearest=1e30,second=1e30;
                uint32_t i=0;
                while(i<settings.primitiveBvh.size()) {
                    const auto& node=settings.primitiveBvh[i];
                    if(tg::renderer::CloudSpatialLowerBound(node,position)>std::min(second,nearest+smoothness)+0.01) { i=node.escape;continue; }
                    for (uint32_t j=node.start;j<node.start+node.count;++j) {
                        const double d=distanceAt(settings.primitives[j],position);
                        if(smoothness==5) ++tested;
                        if(d<nearest) {second=nearest;nearest=d;} else second=std::min(second,d);
                    }
                    ++i;
                }
                const double accelerated=smoothUnion(nearest,second,smoothness);
                accurate &= std::abs(accelerated-reference)<=TG_CLOUD_BVH_ERROR_METERS+1e-5;
                boundedInflation &= reference>=minimum-smoothness*0.25-1e-9 && reference<=minimum+1e-9;
            }
        }
        Check(conservative,"細長い楕円体を含む全階層で距離下界が保守的");
        Check(accurate,"通常unionとsmooth unionの全数評価との差が1mm以下");
        Check(boundedInflation,"smooth unionの膨張が形状数に関係なく滑らかさの1/4以下");
        Check(tested<64*TestPrimitiveCount/2,"離れた形状群では評価する形状数を半分以下へ減らす");
    }

    Section("雲の時間更新");
    {
        tg::renderer::CloudMotion motion;
        motion.Advance(2.0, true, 10.0f, 0.0f);
        Check(std::abs(motion.z-20.0)<1e-5 && motion.x==0, "風速と経過時間に応じて +Z へ進む");
        motion.Advance(5.0, false, 10.0f, 0.0f);
        Check(std::abs(motion.z-20.0)<1e-5, "一時停止中は位置を維持する");
        motion.Advance(1.0, true, 10.0f, 1.570796327f);
        Check(std::abs(motion.x-10.0)<1e-5, "再開と風向変更は現在の位置から続く");
        using tg::renderer::CloudMotion;
        Check(CloudMotion::LocalNoiseOffset(100.0, 100.0, 2) == -100.0f,
              "移動 400m に対し模様は 300m 進み、範囲との相対位置が変わる");
        Check(CloudMotion::LocalNoiseOffset(100.0, 100.0, 0) == 0.0f,
              "従来の全体移動では模様を固定する");
        Check(CloudMotion::LocalNoiseOffset(-100.0, 100.0, 2) == 100.0f,
              "逆風ではノイズの相対移動も反転する");
        Check(CloudMotion::LocalNoiseOffset(10100.0, 100.0, 2) == -100.0f,
              "長時間の移流は両ノイズに共通の周期で折り返す");
        CloudMotion ratioMotion;
        ratioMotion.Advance(2.0, true, 10.0f, 0.0f, 0.75f);
        Check(ratioMotion.driftZ == 5.0, "既定比率では相対移動が風の 25% になる");
        ratioMotion.Advance(1.0, true, 10.0f, 0.0f, 1.0f);
        Check(ratioMotion.driftZ == 5.0, "比率を 1 にしても現在の模様は飛ばず維持する");
        ratioMotion.Advance(1.0, true, 10.0f, 0.0f, 0.0f);
        Check(ratioMotion.driftZ == 15.0, "比率 0 は模様を空間に固定する相対速度になる");
        ratioMotion.Advance(2.0, false, 10.0f, 0.0f, 0.0f);
        Check(ratioMotion.driftZ == 15.0, "停止中は相対移動も止まる");
        ratioMotion.Reset();
        Check(ratioMotion.driftZ == 0.0, "リセットは模様の位相も戻す");
        motion.Reset();
        Check(motion.x==0 && motion.z==0, "開始位置への復帰は移動量を消す");
        NodeGraph graph;
        const auto terrainRevision = graph.TerrainRevision();
        const auto revision = graph.Revision();
        graph.MarkCloudDirty();
        Check(graph.Revision()!=revision && graph.TerrainRevision()==terrainRevision,
              "雲だけの編集は地形の再コンパイルを要求しない");
        graph.MarkDirty();
        Check(graph.TerrainRevision()!=terrainRevision, "通常のグラフ編集は地形を更新する");
    }

    Section("天候層の雲量・雲種マップ");
    {
        NodeGraph graph = NodeGraph::CreateDefault();
        const auto layer = graph.CreateNode(NodeKind::CloudWeatherLayer);
        const auto output = graph.CreateNode(NodeKind::CloudOutput);
        const auto coverageMask = graph.CreateNode(NodeKind::MaskNoise);
        const auto typeMask = graph.CreateNode(NodeKind::MaskNoise);
        Check(graph.FindNode(layer)->inputs.size()==2 && graph.FindNode(layer)->outputs.size()==1,"天候層は Coverage / Type 入力と Volume 出力を持つ");
        Check(graph.CreateLink(graph.FindNode(layer)->outputs.front().id, graph.FindNode(output)->inputs.front().id), "天候層は雲出力へ接続できる");
        auto cloud = graph.CompileCloud();
        Check(cloud.connected && cloud.layer && cloud.weather && cloud.maskPin==0 && cloud.typeMaskPin==0, "未接続の天候層は一様な雲量・雲種");
        Check(cloud.cloud.width==30000.0f && cloud.weatherThickness==5000.0f && cloud.weatherBottom==1500.0f, "既定の範囲・厚さ・雲底を描画設定へ変換");
        Check(cloud.cloud.thickness>5000.0f && cloud.cloud.centerY-cloud.cloud.thickness*0.5f<1500.0f && cloud.cloud.centerY+cloud.cloud.thickness*0.5f==6500.0f, "包囲箱は球殻の沈み分だけ下へ広がり、上端は変わらない");
        Check(cloud.farDistance==20000.0f, "遠景パスの既定開始距離は20km");
        Check(cloud.cloud.coverage==0.4f && std::abs(cloud.cloudType-0.3f)<1e-6f && cloud.cloud.noiseType==1, "雲量・雲種・Perlin-Worley を渡す");
        Check(graph.CreateLink(graph.FindNode(coverageMask)->outputs.front().id, graph.FindNode(layer)->inputs[0].id), "Coverage にマスクを接続できる");
        Check(graph.CreateLink(graph.FindNode(typeMask)->outputs.front().id, graph.FindNode(layer)->inputs[1].id), "Type にマスクを接続できる");
        cloud = graph.CompileCloud();
        Check(cloud.maskNode==coverageMask && cloud.typeMaskNode==typeMask && cloud.maskPin!=cloud.typeMaskPin, "雲量と雲種のマスク元を区別して保持する");
        auto& settings = std::get<tg::graph::CloudWeatherSettings>(graph.FindMutableNode(layer)->settings);
        settings.cloudType = 1.0f; settings.maxThickness = 8000; settings.bottomHeight = 1000;
        graph.MarkCloudDirty();
        cloud = graph.CompileCloud();
        Check(cloud.cloudType==1.0f && cloud.weatherThickness==8000.0f && cloud.weatherBottom==1000.0f, "雲種と厚さの変更を反映する");
        Check(cloud.cloud.noiseSpeedRatio==1.0f, "模様を変化がオフなら模様の速度比は1");
        settings.evolveNoise=true; settings.noiseSpeedRatio=0.25f;
        graph.MarkCloudDirty();
        Check(graph.CompileCloud().cloud.noiseSpeedRatio==0.25f, "模様を変化がオンなら速度比を描画へ渡す");
        const auto compiled = graph.CompileLayersTo(cloud.typeMaskNode, cloud.typeMaskPin);
        Check(!compiled.layers.empty(), "雲種マスクをレイヤー列へコンパイルできる");
    }

    Section("雲層の分布入力");
    {
        NodeGraph graph = NodeGraph::CreateDefault();
        const auto layer = graph.CreateNode(NodeKind::CloudLayer);
        const auto output = graph.CreateNode(NodeKind::CloudOutput);
        const auto mask = graph.CreateNode(NodeKind::MaskNoise);
        const auto layerPin = graph.FindNode(layer)->outputs.front().id;
        const auto inputPin = graph.FindNode(layer)->inputs.front().id;
        const auto maskPin = graph.FindNode(mask)->outputs.front().id;
        Check(graph.CreateLink(layerPin, graph.FindNode(output)->inputs.front().id), "雲層は雲出力へ接続できる");
        Check(graph.CompileCloud().layer && graph.CompileCloud().maskPin == 0, "未接続の雲層は全面分布");
        Check(graph.CreateLink(maskPin, inputPin), "マスクを分布へ接続できる");
        const auto cloud = graph.CompileCloud();
        Check(cloud.maskPin == maskPin && cloud.maskNode == mask, "分布元のピンを保持する");
        Check(cloud.cloud.width == 12000.0f && cloud.cloud.motionMode == 1, "雲層は広い固定範囲が既定");
        Check(cloud.cloud.noiseType == 0, "雲層の既定ノイズは従来の Perlin fBM");
        Check(cloud.cloud.cellCount == 10, "既存の雲層は10セル周期を維持する");
        auto& layerSettings = std::get<tg::graph::CloudNodeSettings>(graph.FindMutableNode(layer)->settings);
        layerSettings.noiseType = 1;
        layerSettings.cellCount = 17;
        graph.MarkCloudDirty();
        Check(graph.CompileCloud().cloud.cellCount == 17, "雲層の繰り返しセル数を描画へ渡す");
        Check(graph.CompileCloud().cloud.noiseType == 1 && graph.CompileCloud().maskPin == maskPin,
            "Perlin-Worley の選択と分布マスクを同時に描画へ渡す");
        const auto compiled = graph.CompileLayersTo(cloud.maskNode, cloud.maskPin);
        Check(!compiled.maskOps.empty(), "分布マスクを既存の評価プログラムへ変換できる");
        Check(!graph.CanCreateLink(layerPin, inputPin), "Volume を分布へ接続しない");
        graph.DeleteNode(mask);
        Check(graph.CompileCloud().maskPin == 0, "分布元の削除で未接続へ戻る");
        Check(graph.CompileLayers().layers.size() == 1, "雲層は地形出力を変えない");
    }

    Section("ノードグラフ — 雲の独立した出力");
    {
        NodeGraph graph = NodeGraph::CreateDefault();
        Check(!graph.CompileCloud().hasOutput, "既存グラフは従来の空設定を使う");
        const auto cloudId = graph.CreateNode(NodeKind::Cloud);
        const auto outputId = graph.CreateNode(NodeKind::CloudOutput);
        const auto* cloud = graph.FindNode(cloudId);
        const auto* output = graph.FindNode(outputId);
        const auto cloudPin = cloud->outputs.front().id;
        const auto inputPin = output->inputs.front().id;
        Check(graph.CompileCloud().hasOutput && !graph.CompileCloud().connected,
              "未接続の雲出力は雲を表示しない");
        Check(!graph.CanCreateLink(graph.Nodes().front().outputs.front().id, inputPin),
              "Material と Volume は接続できない");
        Check(graph.CreateLink(cloudPin,inputPin) && graph.CompileCloud().connected,
              "雲塊を雲出力へ接続できる");
        auto& settings = std::get<tg::graph::CloudNodeSettings>(graph.FindMutableNode(cloudId)->settings);
        settings.centerY = -150.0f;
        settings.width = 900.0f;
        settings.noiseType = 1;
        settings.enabled = false;
        const auto compiled = graph.CompileCloud();
        Check(compiled.cloud.centerY == -150.0f && compiled.cloud.width == 900.0f && !compiled.cloud.enabled,
              "位置・寸法・有効状態が描画用設定へ伝わる");
        Check(compiled.cloud.noiseType == 1, "雲塊にも Perlin-Worley の選択が伝わる");
        Check(graph.CreateNode(NodeKind::CloudOutput) == 0, "雲出力は重複して作れない");
        Check(graph.CompileLayers().layers.size() == 1, "雲の接続は地形の出力へ混入しない");
        graph.DeleteNode(cloudId);
        Check(graph.CompileCloud().hasOutput && !graph.CompileCloud().connected,
              "雲塊を削除すると古い雲が残らない");
        graph.DeleteNode(outputId);
        Check(!graph.CompileCloud().hasOutput, "雲出力を削除すると従来の空設定へ戻る");
    }

    Section("ノードグラフ — 入力のないハイト加工");

    constexpr std::array kOperationKinds = {
        NodeKind::Blur,      NodeKind::Sediment, NodeKind::Crumbling,
        NodeKind::MeanderingRivers, NodeKind::Lake, NodeKind::SnowCover, NodeKind::Snow, NodeKind::River,    NodeKind::Droplet,
        NodeKind::MultiScaleErosion,
        NodeKind::FluvialErosion,
        NodeKind::FlattenBorders,
    };
    for (const NodeKind kind : kOperationKinds) {
        NodeGraph graph;
        const tg::graph::GraphId operationId = graph.CreateNode(kind);
        Check(IsNeutralPlane(graph.CompileLayersTo(operationId)),
              "Base 未接続の加工ノードは変位 0 の平面になる");
    }

    {
        NodeGraph graph;
        const tg::graph::GraphId baseId = graph.CreateNode(NodeKind::Heightmap);
        const tg::graph::GraphId blurId = graph.CreateNode(NodeKind::Blur);
        const tg::graph::Node* base = graph.FindNode(baseId);
        const tg::graph::Node* blur = graph.FindNode(blurId);
        const bool connected = base != nullptr && blur != nullptr && !base->outputs.empty() &&
                               !blur->inputs.empty() &&
                               graph.CreateLink(base->outputs.front().id, blur->inputs.front().id);
        const tg::graph::CompiledGraph compiled = graph.CompileLayersTo(blurId);
        Check(connected && compiled.layers.size() == 2 &&
                  compiled.layers.front().kind == tg::compositor::LayerKind::Shape &&
                  compiled.layers.back().kind == tg::compositor::LayerKind::Blur,
              "Base 接続中の加工ノードは入力と加工を保つ");
    }

    Section("ノードグラフ — Fluvial Erosion の硬度と補助出力");
    {
        NodeGraph graph;
        const auto baseId = graph.CreateNode(NodeKind::Heightmap);
        const auto erosionId = graph.CreateNode(NodeKind::FluvialErosion);
        const auto noiseId = graph.CreateNode(NodeKind::MaskNoise);
        const auto* base = graph.FindNode(baseId);
        const auto* erosion = graph.FindNode(erosionId);
        const auto* noise = graph.FindNode(noiseId);
        Check(erosion->inputs.size() == 3 && erosion->outputs.size() == 4,
              "侵食範囲と硬度を受け、地形・侵食量・堆積量・Age を返す");
        Check(graph.CreateLink(base->outputs[0].id, erosion->inputs[0].id), "地形入力を接続");
        Check(graph.CreateLink(noise->outputs[0].id, erosion->inputs[2].id), "硬度入力を接続");
        const auto compiled = graph.CompileLayersTo(erosionId);
        Check(compiled.layers.size() == 2 && compiled.layers.back().hardnessMaskOp >= 0 &&
              compiled.layers.back().mask.maskOp == -1,
              "侵食範囲が未接続でも硬度を独立した op として評価する");
        for (size_t i = 1; i < 4; ++i) {
            const auto preview = graph.CompileLayersTo(erosionId, erosion->outputs[i].id);
            bool found = false;
            for (const auto& op : preview.maskOps)
                found |= op.kind == tg::compositor::MaskOpKind::FluvialErosion && op.dropletMask.channel == i-1;
            Check(found, "補助出力のプレビューが対応する成分を参照する");
        }
    }

    Section("ノードグラフ — Meandering Rivers のパスと分岐");
    {
        NodeGraph graph;
        const auto baseId = graph.CreateNode(NodeKind::Heightmap);
        const auto riverId = graph.CreateNode(NodeKind::MeanderingRivers);
        const auto pathId = graph.CreateNode(NodeKind::Path);
        const auto surfaceId = graph.CreateNode(NodeKind::Surface);
        const auto* base = graph.FindNode(baseId);
        const auto* river = graph.FindNode(riverId);
        const auto* pathNode = graph.FindNode(pathId);
        const auto* surface = graph.FindNode(surfaceId);
        Check(river->inputs.size() == 2 && river->inputs[1].valueType == tg::graph::ValueType::Path &&
            river->outputs.size() == 2, "地形と Path を受け、Result と River を出す");
        Check(graph.CreateLink(base->outputs[0].id, river->inputs[0].id) &&
            graph.CreateLink(pathNode->outputs[0].id, river->inputs[1].id), "地形と Path を接続できる");
        auto& path = std::get<tg::graph::PathNodeSettings>(graph.FindMutableNode(pathId)->settings).path;
        const auto a = tg::graph::AddPathPoint(path, 0.1f, 0.5f, 0);
        const auto b = tg::graph::AddPathPoint(path, 0.9f, 0.5f, a);
        path.FindPoint(a)->heightOffsetMeters = 2.0f;
        const auto compiled = graph.CompileLayersTo(riverId);
        const auto& samples = compiled.layers.back().meanderPoints;
        Check(!samples.empty() && samples.front().u == 0.1f && samples.back().u == 0.9f &&
            samples.front().along == 0.0f && samples.back().along == 1.0f &&
            samples.front().heightOffset == 2.0f, "向き・端点・高さオフセットを引き継ぐ");
        const auto maskPreview = graph.CompileLayersTo(riverId, river->outputs[1].id);
        bool found = false;
        for (const auto& op : maskPreview.maskOps) found |= op.kind == tg::compositor::MaskOpKind::MeanderingRivers;
        Check(found, "River マスクのプレビューを生成できる");
        Check(graph.CreateLink(base->outputs[0].id, surface->inputs[0].id) &&
            graph.CreateLink(river->outputs[1].id, surface->inputs[1].id), "River だけを別枝で使える");
        const auto branch = graph.CompileLayersTo(surfaceId);
        Check(branch.layers.size() == 3 && branch.layers[1].maskOnly &&
            !branch.layers[1].meanderPoints.empty(), "マスクだけの枝にもパスを渡し、地形を変更しない");
        tg::graph::ReversePathEdge(path, path.edges.front().id);
        const auto reversed = tg::graph::BuildMeanderPoints(path, 1024, 10);
        Check(!reversed.empty() && reversed.front().u == 0.9f && reversed.back().u == 0.1f,
            "ID 順に依存せずパスの矢印方向を使う");
        path.edges.clear();
        Check(graph.CompileLayersTo(riverId).layers.back().meanderPoints.empty(), "Path を空にすると古い川筋を残さない");
        tg::graph::ConnectPathPoints(path, a, b);
        const auto c = tg::graph::AddPathPoint(path, 0.5f, 0.8f, b);
        tg::graph::ConnectPathPoints(path, c, a);
        Check(tg::graph::BuildMeanderPoints(path, 1024, 10).empty(), "閉じた Path を川として評価しない");
    }

    Section("ノードグラフ — Lake の独立した出力");
    {
        NodeGraph graph;
        const auto baseId = graph.CreateNode(NodeKind::Heightmap);
        const auto lakeId = graph.CreateNode(NodeKind::Lake);
        const auto maskId = graph.CreateNode(NodeKind::MaskNoise);
        const auto* base = graph.FindNode(baseId);
        const auto* lake = graph.FindNode(lakeId);
        const auto* mask = graph.FindNode(maskId);
        Check(lake->inputs.size() == 2 && lake->outputs.size() == 4,
              "Base / Mask と Result / Lake / Depth / Water Level を持つ");
        Check(graph.CreateLink(base->outputs[0].id, lake->inputs[0].id) &&
              graph.CreateLink(mask->outputs[0].id, lake->inputs[1].id), "給水範囲を接続できる");
        auto* settings = std::get_if<tg::graph::LayerNodeSettings>(&graph.FindMutableNode(lakeId)->settings);
        settings->layer.lake.allowOutflow = true;
        settings->layer.lake.waterAmount = 3.5f;
        graph.MarkDirty();
        const auto result = graph.CompileLayersTo(lakeId);
        Check(result.layers.size() == 2 && result.layers.back().kind == tg::compositor::LayerKind::Lake &&
              result.layers.back().lake.allowOutflow && result.layers.back().lake.waterAmount == 3.5f &&
              result.layers.back().mask.maskOp >= 0, "専用設定と給水マスクを保持する");
        for (size_t i = 1; i < 4; ++i) {
            const auto preview = graph.CompileLayersTo(lakeId, lake->outputs[i].id);
            bool found = false;
            for (const auto& op : preview.maskOps)
                found |= op.kind == tg::compositor::MaskOpKind::Lake && op.dropletMask.channel == i - 1;
            Check(found, "湖・水深・水位を取り違えずプレビューする");
        }
    }

    Section("ノードグラフ — Snow Cover の独立した出力");
    {
        NodeGraph graph;
        const auto baseId = graph.CreateNode(NodeKind::Heightmap);
        const auto snowId = graph.CreateNode(NodeKind::SnowCover);
        const auto maskId = graph.CreateNode(NodeKind::MaskNoise);
        const auto* base = graph.FindNode(baseId);
        const auto* snow = graph.FindNode(snowId);
        const auto* mask = graph.FindNode(maskId);
        Check(snow->inputs.size() == 2 && snow->outputs.size() == 4,
              "Base / Mask と Result / Cover / Depth / Flows を持つ");
        Check(graph.CreateLink(base->outputs[0].id, snow->inputs[0].id) &&
              graph.CreateLink(mask->outputs[0].id, snow->inputs[1].id), "降雪範囲を接続できる");
        auto* settings = std::get_if<tg::graph::LayerNodeSettings>(&graph.FindMutableNode(snowId)->settings);
        settings->layer.snowCover.dusting = true;
        settings->layer.snowCover.snowfallDepth = 3.5f;
        graph.MarkDirty();
        const auto result = graph.CompileLayersTo(snowId);
        Check(result.layers.size() == 2 && result.layers.back().kind == tg::compositor::LayerKind::SnowCover &&
              result.layers.back().snowCover.dusting && result.layers.back().snowCover.snowfallDepth == 3.5f &&
              result.layers.back().mask.maskOp >= 0, "専用設定と降雪マスクを保持する");
        for (size_t i = 1; i < 4; ++i) {
            const auto preview = graph.CompileLayersTo(snowId, snow->outputs[i].id);
            bool found = false;
            for (const auto& op : preview.maskOps)
                found |= op.kind == tg::compositor::MaskOpKind::SnowCover && op.dropletMask.channel == i - 1;
            Check(found, "被覆・雪深・流動量を取り違えずプレビューする");
        }
    }

    Section("ノードグラフ — Sediment の Emission 入力");
    {
        // Emission に繋いだマスクは、堆積レイヤーの Mask 入力（供給元）として op へ落ちる。
        NodeGraph graph;
        const tg::graph::GraphId baseId = graph.CreateNode(NodeKind::Heightmap);
        const tg::graph::GraphId noiseId = graph.CreateNode(NodeKind::MaskNoise);
        const tg::graph::GraphId sedimentId = graph.CreateNode(NodeKind::Sediment);
        const tg::graph::Node* base = graph.FindNode(baseId);
        const tg::graph::Node* noise = graph.FindNode(noiseId);
        const tg::graph::Node* sediment = graph.FindNode(sedimentId);
        const bool hasPins = base != nullptr && noise != nullptr && sediment != nullptr &&
                             !base->outputs.empty() && !noise->outputs.empty() &&
                             sediment->inputs.size() == 2 &&
                             sediment->inputs[1].valueType == tg::graph::ValueType::Mask;
        const bool connected =
            hasPins && graph.CreateLink(base->outputs.front().id, sediment->inputs[0].id) &&
            graph.CreateLink(noise->outputs.front().id, sediment->inputs[1].id);
        const tg::graph::CompiledGraph compiled = graph.CompileLayersTo(sedimentId);
        const bool wired = compiled.layers.size() == 2 &&
                           compiled.layers.back().kind == tg::compositor::LayerKind::Sediment &&
                           compiled.layers.back().mask.source == tg::compositor::MaskSource::Node &&
                           compiled.layers.back().mask.maskOp >= 0 &&
                           static_cast<size_t>(compiled.layers.back().mask.maskOp) <
                               compiled.maskOps.size();
        Check(connected && wired, "Sediment の Emission 入力は供給元のマスク op になる");

        // 繋がなければ供給元は無し（全面へ一様）。
        NodeGraph plain;
        const tg::graph::GraphId plainBaseId = plain.CreateNode(NodeKind::Heightmap);
        const tg::graph::GraphId plainSedimentId = plain.CreateNode(NodeKind::Sediment);
        const tg::graph::Node* plainBase = plain.FindNode(plainBaseId);
        const tg::graph::Node* plainSediment = plain.FindNode(plainSedimentId);
        const bool plainConnected =
            plainBase != nullptr && plainSediment != nullptr && !plainBase->outputs.empty() &&
            !plainSediment->inputs.empty() &&
            plain.CreateLink(plainBase->outputs.front().id, plainSediment->inputs[0].id);
        const tg::graph::CompiledGraph plainCompiled = plain.CompileLayersTo(plainSedimentId);
        Check(plainConnected && plainCompiled.layers.size() == 2 &&
                  plainCompiled.layers.back().mask.source != tg::compositor::MaskSource::Node,
              "Emission 未接続の Sediment は供給元を持たない");
    }

    Section("ノードグラフ — Snow の Mask 入力");
    {
        // Mask に繋いだマスクは、積雪レイヤーの Mask 入力（降らせる場所）として op へ落ちる。
        NodeGraph graph;
        const tg::graph::GraphId baseId = graph.CreateNode(NodeKind::Heightmap);
        const tg::graph::GraphId heightId = graph.CreateNode(NodeKind::MaskHeight);
        const tg::graph::GraphId snowId = graph.CreateNode(NodeKind::Snow);
        const tg::graph::Node* base = graph.FindNode(baseId);
        const tg::graph::Node* height = graph.FindNode(heightId);
        const tg::graph::Node* snow = graph.FindNode(snowId);
        const bool hasPins = base != nullptr && height != nullptr && snow != nullptr &&
                             !base->outputs.empty() && !height->outputs.empty() &&
                             snow->inputs.size() == 2 &&
                             snow->inputs[1].valueType == tg::graph::ValueType::Mask;
        const bool connected =
            hasPins && graph.CreateLink(base->outputs.front().id, snow->inputs[0].id) &&
            graph.CreateLink(height->outputs.front().id, snow->inputs[1].id);
        const tg::graph::CompiledGraph compiled = graph.CompileLayersTo(snowId);
        const bool wired = compiled.layers.size() == 2 &&
                           compiled.layers.back().kind == tg::compositor::LayerKind::Snow &&
                           compiled.layers.back().mask.source == tg::compositor::MaskSource::Node &&
                           compiled.layers.back().mask.maskOp >= 0 &&
                           static_cast<size_t>(compiled.layers.back().mask.maskOp) <
                               compiled.maskOps.size();
        Check(connected && wired, "Snow の Mask 入力は降らせる場所のマスク op になる");

        // 繋がなければ全面へ一様。
        NodeGraph plain;
        const tg::graph::GraphId plainBaseId = plain.CreateNode(NodeKind::Heightmap);
        const tg::graph::GraphId plainSnowId = plain.CreateNode(NodeKind::Snow);
        const tg::graph::Node* plainBase = plain.FindNode(plainBaseId);
        const tg::graph::Node* plainSnow = plain.FindNode(plainSnowId);
        const bool plainConnected =
            plainBase != nullptr && plainSnow != nullptr && !plainBase->outputs.empty() &&
            !plainSnow->inputs.empty() &&
            plain.CreateLink(plainBase->outputs.front().id, plainSnow->inputs[0].id);
        const tg::graph::CompiledGraph plainCompiled = plain.CompileLayersTo(plainSnowId);
        Check(plainConnected && plainCompiled.layers.size() == 2 &&
                  plainCompiled.layers.back().mask.source != tg::compositor::MaskSource::Node,
              "Mask 未接続の Snow は降らせる場所を持たない");
    }

    Section("パス — まとめて動かす / コピーと貼り付け");
    {
        using tg::graph::PathClip;
        using tg::graph::PathElementId;
        using tg::graph::PathSettings;
        PathSettings path;
        const PathElementId a = tg::graph::AddPathPoint(path, 0.2f, 0.2f, 0);
        const PathElementId b = tg::graph::AddPathPoint(path, 0.4f, 0.2f, a);
        const PathElementId c = tg::graph::AddPathPoint(path, 0.4f, 0.4f, b);
        const PathElementId lone = tg::graph::AddPathPoint(path, 0.9f, 0.9f, 0);
        const tg::graph::PathEdge* ab = path.FindEdgeBetween(a, b);
        const tg::graph::PathEdge* bc = path.FindEdgeBetween(b, c);
        Check(ab != nullptr && bc != nullptr && lone != 0, "3 点の鎖と孤立点を作れる");

        // まとめて動かす。0〜1 へ丸める。
        const bool moved = tg::graph::MovePathPoints(path, {a, b, c}, 0.1f, -0.3f);
        const tg::graph::PathPoint* pa = path.FindPoint(a);
        const tg::graph::PathPoint* pc = path.FindPoint(c);
        Check(moved && pa != nullptr && pc != nullptr && std::abs(pa->u - 0.3f) < 1e-5f &&
                  pa->v == 0.0f && std::abs(pc->u - 0.5f) < 1e-5f && std::abs(pc->v - 0.1f) < 1e-5f,
              "MovePathPoints は指定した点だけを動かし、0〜1 へ丸める");
        float cu = 0.0f;
        float cv = 0.0f;
        Check(tg::graph::PathPointsCentroid(path, {a, b, c}, cu, cv) &&
                  std::abs(cu - (0.3f + 0.5f + 0.5f) / 3.0f) < 1e-5f,
              "PathPointsCentroid は重心を返す");

        // 鎖を切り出す。エッジは両端の点を連れていき、内部点は持ち越さない。
        if (ab != nullptr && bc != nullptr) {
            tg::graph::PathEdge* mutableAb = const_cast<tg::graph::PathEdge*>(ab);
            mutableAb->routed = true;
            mutableAb->waypoints.push_back({0.35f, 0.1f});
            mutableAb->curve = tg::graph::PathCurve::Cubic;
        }
        PathClip clip;
        const bool extracted = tg::graph::ExtractPathClip(path, {}, {ab->id, bc->id}, clip);
        Check(extracted && clip.points.size() == 3 && clip.edges.size() == 2 &&
                  !clip.edges.front().routed && clip.edges.front().waypoints.empty() &&
                  clip.edges.front().curve == tg::graph::PathCurve::Cubic,
              "ExtractPathClip は鎖の点とエッジを切り出し、内部点は捨てて曲線の性質は残す");

        // 点の集合から切り出すと、その間のエッジだけが付いてくる。
        PathClip pointClip;
        Check(tg::graph::ExtractPathClip(path, {a, b, lone}, {}, pointClip) &&
                  pointClip.points.size() == 3 && pointClip.edges.size() == 1,
              "点の集合の ExtractPathClip は点どうしを結ぶエッジだけを拾う");

        // 貼り付け。ID は振り直され、ずらした位置に同じ形で入る。
        const size_t pointsBefore = path.points.size();
        const size_t edgesBefore = path.edges.size();
        std::vector<PathElementId> pastedPoints;
        std::vector<PathElementId> pastedEdges;
        const bool pasted =
            tg::graph::PastePathClip(path, clip, 0.2f, 0.5f, &pastedPoints, &pastedEdges);
        bool idsFresh = true;
        for (const PathElementId id : pastedPoints) {
            idsFresh &= (id != a && id != b && id != c && id != lone);
        }
        const tg::graph::PathPoint* firstPasted =
            pastedPoints.empty() ? nullptr : path.FindPoint(pastedPoints.front());
        Check(pasted && path.points.size() == pointsBefore + 3 &&
                  path.edges.size() == edgesBefore + 2 && pastedEdges.size() == 2 && idsFresh &&
                  firstPasted != nullptr && std::abs(firstPasted->u - 0.5f) < 1e-5f &&
                  std::abs(firstPasted->v - 0.5f) < 1e-5f &&
                  path.FindEdgeBetween(pastedPoints[0], pastedPoints[1]) != nullptr,
              "PastePathClip は新しい ID で同じ形を、ずらした位置に貼る");
        Check(tg::graph::BuildPathStrands(path).size() == 2,
              "貼った鎖は元の鎖と別の鎖になる");
    }

    Section("パス — 面の線分列と Mask Area");
    {
        using tg::graph::PathElementId;
        using tg::graph::PathSettings;
        // 開いた鎖だけなら面の線分は無い。
        PathSettings open;
        const PathElementId o1 = tg::graph::AddPathPoint(open, 0.2f, 0.2f, 0);
        const PathElementId o2 = tg::graph::AddPathPoint(open, 0.8f, 0.2f, o1);
        tg::graph::AddPathPoint(open, 0.8f, 0.8f, o2);
        Check(tg::graph::BuildPathAreaSegments(open).empty(),
              "開いた鎖だけの BuildPathAreaSegments は空");

        // 三角形の輪。線分は輪を一周して先頭へ戻る。
        PathSettings loop;
        const PathElementId a = tg::graph::AddPathPoint(loop, 0.2f, 0.2f, 0);
        const PathElementId b = tg::graph::AddPathPoint(loop, 0.8f, 0.2f, a);
        const PathElementId c = tg::graph::AddPathPoint(loop, 0.5f, 0.8f, b);
        tg::graph::ConnectPathPoints(loop, c, a);
        const auto segments = tg::graph::BuildPathAreaSegments(loop);
        bool chained = !segments.empty();
        for (size_t i = 0; i + 1 < segments.size(); ++i) {
            chained &= (segments[i].bx == segments[i + 1].ax && segments[i].by == segments[i + 1].ay);
        }
        const bool closed = !segments.empty() && segments.back().bx == segments.front().ax &&
                            segments.back().by == segments.front().ay;
        Check(segments.size() == 3 && chained && closed,
              "閉じた鎖の BuildPathAreaSegments は輪を一周して先頭へ戻る");

        // グラフ: Path → Mask Area → Surface の Mask。閉じた鎖があれば Area の op になる。
        NodeGraph graph;
        const tg::graph::GraphId baseId = graph.CreateNode(NodeKind::Heightmap);
        const tg::graph::GraphId pathId = graph.CreateNode(NodeKind::Path);
        const tg::graph::GraphId areaId = graph.CreateNode(NodeKind::MaskArea);
        const tg::graph::GraphId surfaceId = graph.CreateNode(NodeKind::Surface);
        tg::graph::Node* pathNode = graph.FindMutableNode(pathId);
        if (auto* settings = std::get_if<tg::graph::PathNodeSettings>(&pathNode->settings)) {
            settings->path = loop;
        }
        const tg::graph::Node* base = graph.FindNode(baseId);
        const tg::graph::Node* area = graph.FindNode(areaId);
        const tg::graph::Node* surface = graph.FindNode(surfaceId);
        const bool linked =
            graph.CreateLink(pathNode->outputs.front().id, area->inputs.front().id) &&
            graph.CreateLink(base->outputs.front().id, surface->inputs[0].id) &&
            graph.CreateLink(area->outputs.front().id, surface->inputs[1].id);
        const tg::graph::CompiledGraph compiled = graph.CompileLayersTo(surfaceId);
        const bool wired = compiled.layers.size() == 2 &&
                           compiled.layers.back().mask.source == tg::compositor::MaskSource::Node &&
                           compiled.layers.back().mask.maskOp >= 0 &&
                           static_cast<size_t>(compiled.layers.back().mask.maskOp) <
                               compiled.maskOps.size() &&
                           compiled.maskOps[static_cast<size_t>(compiled.layers.back().mask.maskOp)]
                                   .kind == tg::compositor::MaskOpKind::Area &&
                           compiled.maskOps.front().pathSegments.size() == 3;
        Check(linked && wired, "Mask Area は閉じた鎖から Area の op になる");

        // 開いた鎖しか無ければ op は作られない（マスクは定数へ落ちる）。
        if (auto* settings = std::get_if<tg::graph::PathNodeSettings>(&pathNode->settings)) {
            settings->path = open;
        }
        graph.MarkDirty();
        const tg::graph::CompiledGraph openCompiled = graph.CompileLayersTo(surfaceId);
        Check(openCompiled.layers.size() == 2 &&
                  openCompiled.layers.back().mask.source != tg::compositor::MaskSource::Node,
              "閉じた鎖が無い Mask Area は op を作らない");
    }

    Section("ノードグラフ — Path の Base");
    {
        NodeGraph graph;
        const tg::graph::GraphId heightmapId = graph.CreateNode(NodeKind::Heightmap);
        const tg::graph::GraphId outputId = graph.CreateNode(NodeKind::Output);
        const tg::graph::GraphId pathId = graph.CreateNode(NodeKind::Path);
        const tg::graph::Node* heightmap = graph.FindNode(heightmapId);
        const tg::graph::Node* output = graph.FindNode(outputId);
        const tg::graph::Node* path = graph.FindNode(pathId);

        const bool outputConnected =
            heightmap != nullptr && output != nullptr && !heightmap->outputs.empty() &&
            !output->inputs.empty() &&
            graph.CreateLink(heightmap->outputs.front().id, output->inputs.front().id);
        Check(outputConnected && IsNeutralPlane(graph.CompileLayersTo(pathId)),
              "Base 未接続の Path は Output 側の地形ではなく変位 0 の平面になる");

        const bool pathConnected =
            heightmap != nullptr && path != nullptr && !heightmap->outputs.empty() &&
            !path->inputs.empty() &&
            graph.CreateLink(heightmap->outputs.front().id, path->inputs.front().id);
        const tg::graph::CompiledGraph compiled = graph.CompileLayersTo(pathId);
        Check(pathConnected && compiled.layers.size() == 1 &&
                  compiled.layers.front().kind == tg::compositor::LayerKind::Shape &&
                  compiled.layers.front().heightSource != tg::compositor::ValueSource::Constant,
              "Base 接続中の Path は自身の入力地形を表示する");
    }

    Section("ノードグラフ — Mask Flowline の入力とマスク出力");
    {
        NodeGraph graph;
        const auto baseId = graph.CreateNode(NodeKind::Heightmap);
        const auto flowId = graph.CreateNode(NodeKind::MaskFlowline);
        const auto sourceId = graph.CreateNode(NodeKind::MaskNoise);
        const auto outflowId = graph.CreateNode(NodeKind::MaskNoise);
        const auto* base = graph.FindNode(baseId);
        const auto* flow = graph.FindNode(flowId);
        const auto* source = graph.FindNode(sourceId);
        const auto* outflow = graph.FindNode(outflowId);
        Check(flow->inputs.size() == 3 && flow->outputs.size() == 1 &&
              flow->outputs[0].valueType == tg::graph::ValueType::Mask,
              "Base / Source / Outflow を受け、Mask だけを出す");
        Check(graph.CreateLink(base->outputs[0].id, flow->inputs[0].id) &&
              graph.CreateLink(source->outputs[0].id, flow->inputs[1].id) &&
              graph.CreateLink(outflow->outputs[0].id, flow->inputs[2].id), "地形・発生範囲・流出域を接続できる");
        auto& settings = std::get<tg::graph::MaskNodeSettings>(graph.FindMutableNode(flowId)->settings);
        settings.flowline.numberOfFlows = 321;
        settings.flowline.lengthMeters = 45.0f;
        graph.MarkDirty();
        const auto compiled = graph.CompileLayersTo(flowId);
        Check(compiled.maskOps.size() == 3, "依存マスクを先にコンパイルする");
        const auto& op = compiled.maskOps.back();
        Check(op.kind == tg::compositor::MaskOpKind::Flowline && op.inputA == 0 && op.inputB == 1 &&
              op.heightSourceLayer == 0 && op.flowline.numberOfFlows == 321 && op.flowline.lengthMeters == 45.0f,
              "入力の順序・地形の参照位置・専用設定を保持する");
        bool heightUnchanged = true;
        for (size_t i = 1; i < compiled.layers.size(); ++i)
            heightUnchanged &= (compiled.layers[i].channelMask & tg::compositor::ChannelBit(tg::compositor::Channel::Height)) == 0;
        Check(heightUnchanged, "マスクのプレビューは地形の高さを書き換えない");
        Check(!graph.CreateLink(flow->outputs[0].id, flow->inputs[1].id), "自分の出力を発生範囲に戻す循環を拒否する");
        graph.DeleteNode(sourceId);
        const auto disconnected = graph.CompileLayersTo(flowId);
        Check(disconnected.maskOps.back().inputA == -1 && disconnected.maskOps.back().inputB == 0,
              "発生範囲を外しても流出域の接続は保持する");
    }

    Section("ノードグラフ — ハイト由来マスクの Base");
    constexpr std::array kHeightMaskKinds = {
        NodeKind::MaskFlowline,
        NodeKind::MaskFluvial,
        NodeKind::MaskHeight,
        NodeKind::MaskSlope,
        NodeKind::MaskCurvature,
    };
    for (const NodeKind kind : kHeightMaskKinds) {
        NodeGraph graph;
        const tg::graph::GraphId heightmapId = graph.CreateNode(NodeKind::Heightmap);
        const tg::graph::GraphId outputId = graph.CreateNode(NodeKind::Output);
        const tg::graph::GraphId maskId = graph.CreateNode(kind);
        const tg::graph::Node* heightmap = graph.FindNode(heightmapId);
        const tg::graph::Node* output = graph.FindNode(outputId);
        const tg::graph::Node* mask = graph.FindNode(maskId);

        const bool outputConnected =
            heightmap != nullptr && output != nullptr && !heightmap->outputs.empty() &&
            !output->inputs.empty() &&
            graph.CreateLink(heightmap->outputs.front().id, output->inputs.front().id);
        const tg::graph::CompiledGraph disconnected = graph.CompileLayersTo(maskId);
        Check(outputConnected && StartsWithNeutralPlane(disconnected),
              "Base 未接続のハイト由来マスクは変位 0 の平面上で表示する");

        const bool maskConnected =
            heightmap != nullptr && mask != nullptr && !heightmap->outputs.empty() &&
            !mask->inputs.empty() &&
            graph.CreateLink(heightmap->outputs.front().id, mask->inputs.front().id);
        const tg::graph::CompiledGraph connected = graph.CompileLayersTo(maskId);
        Check(maskConnected && !connected.layers.empty() &&
                  connected.layers.front().kind == tg::compositor::LayerKind::Shape &&
                  connected.layers.front().heightSource != tg::compositor::ValueSource::Constant,
              "Base 接続中のハイト由来マスクは自身の入力地形上で表示する");
    }
}
