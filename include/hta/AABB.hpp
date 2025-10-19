//
// Information extracted with resym v0.4.0
//
// PDB file: C:\Program Files (x86)\Steam\steamapps\common\Hard Truck Apocalypse\game.pdb
// Image architecture: X86
//

#pragma once
#include <cstdint>
#include <array>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct CVector;
class Aabb;

class Aabb { /* Size=0x18 */
  /* 0x0000 */ public: float m_box[6];
  
  public: void Create(const CVector&, const CVector&);
  public: void Offset(const CVector&);
  public: void Scale(const CVector&);
  public: float GetSz() const;
  public: float GetSy() const;
  public: float GetSx() const;
  public: void Inflate(float);
  public: void StartEmbracing();
  public: void EmbracePoint(const CVector&);
  public: void EmbraceBox(const Aabb&);
  public: CVector Min() const;
  public: CVector Max() const;
  public: float MaximumComponent() const;
  public: bool IsPtInside(const CVector&) const;
  public: bool IsPtInside2(const CVector&) const;
  public: void Draw(uint32_t);
};
