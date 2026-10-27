#ifndef MODEL_HPP
#define MODEL_HPP

class ModelListener;

enum class DistanceUnit { Meters, Centimeters };

class Model
{
public:
    Model();

    void bind(ModelListener* listener)
    {
        modelListener = listener;
    }

    void tick();

    DistanceUnit getDistanceUnit() const { return distanceUnit; }
    void setDistanceUnit(DistanceUnit unit) { distanceUnit = unit; }
    
    void toggleGpio();
protected:
    ModelListener* modelListener;
    // Local display preference; survives screen changes until the next boot.
    DistanceUnit distanceUnit;
};

#endif // MODEL_HPP
